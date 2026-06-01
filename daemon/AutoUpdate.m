#import "daemon/AutoUpdate.h"
#import "daemon/RDPServer.h"
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <unistd.h>
#import <stdlib.h>
#import <sys/stat.h>
#define RDP_LOG_COMPONENT "update"
#include "logging/RDPLog.h"

/*
 * SILENT AUTO-UPDATER — design notes
 * ----------------------------------------------------------------------------
 * THREADING: the daemon's main thread is permanently parked in kevent() (see
 * daemon/main.m), so there is no running main run loop. We therefore drive the
 * periodic check from a dispatch_source TIMER on a private background SERIAL
 * queue (same pattern as display/ScreenCapture.m). All work — networking,
 * codesign, file IO, restart — happens on that queue; never on the main queue,
 * never via NSTimer.
 *
 * FAIL-SAFE: every step is wrapped so a failure only logs + returns; the next
 * interval retries. The live binary is only ever touched at the very last step
 * (atomic rename of a fully-downloaded, sha-verified, re-signed temp file). A
 * .bak of the previous binary is kept. If anything before the swap fails the
 * temp file is deleted and the live binary is untouched.
 *
 * CONFIG: see AutoUpdate.h for the full environment-variable list.
 */

/* ── Defaults ─────────────────────────────────────────────────────────── */
#define DEF_INTERVAL_MIN   5
#define DEF_REPO           "grioghar/macos-rdp-server"
#define DEF_SIGN_KEYCHAIN  "signing/macos-rdp.keychain-db"  /* relative to ~/.macos-rdp */
#define DEF_SIGN_PW        "macosrdp"
#define DEF_SIGN_IDENTITY  "5A4065DBD72516AFC58960B05B2A135B32650D44"
#define ASSET_BIN          "macos-rdp-daemon"
#define ASSET_SHA          "macos-rdp-daemon.sha256"
#define NET_TIMEOUT_SEC    30.0

@interface AutoUpdate ()
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, strong) dispatch_source_t timer;
@property (nonatomic, strong) NSURLSession *session;
@property (nonatomic, assign) BOOL checking;   /* re-entrancy guard (queue-local) */
@end

@implementation AutoUpdate

static AutoUpdate *g_updater = nil;

/* ── env helpers ──────────────────────────────────────────────────────── */
static NSString *envStr(const char *name, NSString *def) {
    const char *v = getenv(name);
    if (v && *v) return [NSString stringWithUTF8String:v];
    return def;
}

/* ~/.macos-rdp absolute base (HOME is set by main.m before we run). */
static NSString *baseDir(void) {
    NSString *home = NSHomeDirectory();
    if (home.length == 0) home = @"/var/root";
    return [home stringByAppendingPathComponent:@".macos-rdp"];
}
static NSString *binPath(void)  { return [baseDir() stringByAppendingPathComponent:@"bin/macos-rdp-daemon"]; }
static NSString *tmpPath(void)  { return [baseDir() stringByAppendingPathComponent:@"bin/.update.tmp"]; }
static NSString *bakPath(void)  { return [baseDir() stringByAppendingPathComponent:@"bin/macos-rdp-daemon.bak"]; }

static NSString *signKeychain(void) {
    const char *v = getenv("RDP_SIGN_KEYCHAIN");
    if (v && *v) return [NSString stringWithUTF8String:v];
    return [baseDir() stringByAppendingPathComponent:@DEF_SIGN_KEYCHAIN];
}

/* ── lifecycle ────────────────────────────────────────────────────────── */
+ (void)start {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (![envStr("RDP_UPDATE_ENABLED", @"1") isEqualToString:@"1"]) {
            rdp_info("auto-update disabled (RDP_UPDATE_ENABLED=0)");
            return;
        }
        g_updater = [[AutoUpdate alloc] init];
        [g_updater run];
    });
}

+ (void)stop {
    AutoUpdate *u = g_updater;
    if (!u) return;
    dispatch_async(u.queue, ^{
        if (u.timer) { dispatch_source_cancel(u.timer); u.timer = nil; }
    });
}

- (void)run {
    self.queue = dispatch_queue_create("com.macosrdp.autoupdate", DISPATCH_QUEUE_SERIAL);

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest  = NET_TIMEOUT_SEC;
    cfg.timeoutIntervalForResource = NET_TIMEOUT_SEC * 4;
    cfg.waitsForConnectivity       = NO;
    self.session = [NSURLSession sessionWithConfiguration:cfg];

    int mins = (int)[envStr("RDP_UPDATE_INTERVAL_MIN",
                            [NSString stringWithFormat:@"%d", DEF_INTERVAL_MIN]) intValue];
    if (mins < 1) mins = DEF_INTERVAL_MIN;
    uint64_t intervalNs = (uint64_t)mins * 60ull * NSEC_PER_SEC;

    rdp_info("auto-update started: repo=%s interval=%dm running=%s",
             envStr("RDP_UPDATE_REPO", @DEF_REPO).UTF8String, mins,
             MACOS_RDP_BUILD_VERSION);

    __weak typeof(self) weak = self;
    self.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    /* Immediate first fire, then every `mins` minutes; generous leeway since
     * timing is not critical and we want the scheduler to coalesce wakeups. */
    dispatch_source_set_timer(self.timer,
                              dispatch_time(DISPATCH_TIME_NOW, 0),
                              intervalNs,
                              30ull * NSEC_PER_SEC);
    dispatch_source_set_event_handler(self.timer, ^{
        [weak tick];
    });
    dispatch_resume(self.timer);
}

/* Runs on self.queue. Wrapped so no exception can escape onto the queue. */
- (void)tick {
    if (self.checking) return;          /* a slow check is still running */
    self.checking = YES;
    @try {
        [self checkOnce];
    } @catch (NSException *e) {
        rdp_error("auto-update tick exception: %s", e.reason.UTF8String);
    }
    self.checking = NO;
}

/* ── one check cycle (synchronous on self.queue) ──────────────────────── */
- (void)checkOnce {
    NSString *repo = envStr("RDP_UPDATE_REPO", @DEF_REPO);
    NSString *apiURL = [NSString stringWithFormat:
        @"https://api.github.com/repos/%@/releases/latest", repo];

    NSDictionary *release = [self getJSON:apiURL];
    if (![release isKindOfClass:[NSDictionary class]]) {
        rdp_verbose("update check: no release JSON (network/parse) — retry next interval");
        return;
    }

    NSString *remoteVer = release[@"tag_name"];
    if (![remoteVer isKindOfClass:[NSString class]] || remoteVer.length == 0) {
        rdp_verbose("update check: release has no tag_name");
        return;
    }

    NSString *localVer = @MACOS_RDP_BUILD_VERSION;
    if ([remoteVer isEqualToString:localVer]) {
        rdp_verbose("up to date (version %s)", localVer.UTF8String);
        return;
    }

    /* An update exists. Don't disturb a connected user — apply only when idle. */
    if ([RDPServer hasActiveSession]) {
        rdp_info("update %s available, deferring until idle", remoteVer.UTF8String);
        return;
    }

    rdp_info("update available: %s -> %s; applying", localVer.UTF8String, remoteVer.UTF8String);

    /* Locate the binary asset + its sha256 (asset or release body). */
    NSString *binURL = nil, *shaURL = nil;
    NSArray *assets = release[@"assets"];
    if ([assets isKindOfClass:[NSArray class]]) {
        for (NSDictionary *a in assets) {
            if (![a isKindOfClass:[NSDictionary class]]) continue;
            NSString *name = a[@"name"];
            NSString *url  = a[@"browser_download_url"];
            if (![name isKindOfClass:[NSString class]] || ![url isKindOfClass:[NSString class]]) continue;
            if ([name isEqualToString:@ASSET_BIN]) binURL = url;
            else if ([name isEqualToString:@ASSET_SHA]) shaURL = url;
        }
    }
    if (!binURL) {
        rdp_error("update %s: no '%s' asset in release — skipping", remoteVer.UTF8String, ASSET_BIN);
        return;
    }

    /* Expected SHA: prefer the .sha256 asset, else scan the release body. */
    NSString *expectedSha = nil;
    if (shaURL) expectedSha = [self fetchSha256FromURL:shaURL];
    if (!expectedSha) {
        NSString *body = release[@"body"];
        if ([body isKindOfClass:[NSString class]]) expectedSha = [self firstSha256InString:body];
    }
    if (!expectedSha) {
        rdp_error("update %s: no published sha256 (asset or body) — refusing unverified binary", remoteVer.UTF8String);
        return;
    }

    /* Download to temp. */
    NSString *tmp = tmpPath();
    [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
    if (![self downloadURL:binURL toPath:tmp]) {
        rdp_error("update %s: download failed", remoteVer.UTF8String);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
        return;
    }

    /* Verify SHA-256. */
    NSString *gotSha = [self sha256OfFile:tmp];
    if (![gotSha isEqualToString:[expectedSha lowercaseString]]) {
        rdp_error("update %s: sha256 mismatch (expected %s got %s) — aborting",
                  remoteVer.UTF8String, expectedSha.UTF8String, gotSha.UTF8String);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
        return;
    }
    rdp_info("update %s: sha256 verified", remoteVer.UTF8String);

    /* Re-sign locally so TCC grants survive. */
    if (![self resignBinary:tmp]) {
        rdp_error("update %s: re-sign failed — aborting (live binary untouched)", remoteVer.UTF8String);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
        return;
    }

    /* chmod 755. */
    if (chmod(tmp.fileSystemRepresentation, 0755) != 0) {
        rdp_error("update %s: chmod 755 failed: %s — aborting", remoteVer.UTF8String, strerror(errno));
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
        return;
    }

    /* Re-check idle one last time right before the swap (a client may have
     * connected during the download). */
    if ([RDPServer hasActiveSession]) {
        rdp_info("update %s: client connected during download, deferring until idle", remoteVer.UTF8String);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];
        return;
    }

    /* Atomic swap: keep a .bak of the current binary, then rename temp over it. */
    NSString *live = binPath();
    NSString *bak  = bakPath();
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtPath:bak error:NULL];
    if ([fm fileExistsAtPath:live] && ![fm copyItemAtPath:live toPath:bak error:NULL])
        rdp_error("update %s: could not back up live binary (continuing)", remoteVer.UTF8String);

    /* rename(2) is atomic on the same filesystem; replaces the live inode. */
    if (rename(tmp.fileSystemRepresentation, live.fileSystemRepresentation) != 0) {
        rdp_error("update %s: atomic swap failed: %s — aborting", remoteVer.UTF8String, strerror(errno));
        [fm removeItemAtPath:tmp error:NULL];
        return;
    }
    rdp_info("update %s: swapped in new binary; restarting", remoteVer.UTF8String);

    [self restart];
}

/* ── restart ──────────────────────────────────────────────────────────── */
- (void)restart {
    uid_t uid = getuid();
    NSString *target = [NSString stringWithFormat:@"gui/%u/com.macosrdp.agent.user", uid];
    int rc = [self runTask:@"/bin/launchctl"
                      args:@[@"kickstart", @"-k", target]
                   timeout:15.0];
    if (rc == 0) {
        rdp_info("launchctl kickstart issued for %s; exiting to relaunch", target.UTF8String);
    } else {
        rdp_error("launchctl kickstart failed (rc=%d); exiting so launchd KeepAlive relaunches", rc);
    }
    /* Either way, exit cleanly. With kickstart -k launchd kills+restarts us;
     * if that failed, KeepAlive relaunches us on exit. */
    exit(0);
}

/* ── re-sign ──────────────────────────────────────────────────────────── */
- (BOOL)resignBinary:(NSString *)path {
    NSString *keychain = signKeychain();
    NSString *pw       = envStr("RDP_SIGN_KEYCHAIN_PW", @DEF_SIGN_PW);
    NSString *identity = envStr("RDP_SIGN_IDENTITY", @DEF_SIGN_IDENTITY);

    /* 1. Unlock the signing keychain. */
    if ([self runTask:@"/usr/bin/security"
                 args:@[@"unlock-keychain", @"-p", pw, keychain]
              timeout:15.0] != 0) {
        rdp_error("re-sign: unlock-keychain failed for %s", keychain.UTF8String);
        return NO;
    }

    /* 2. Sign, pinning the stable identifier so existing TCC grants apply. */
    if ([self runTask:@"/usr/bin/codesign"
                 args:@[@"--keychain", keychain, @"-s", identity,
                        @"--identifier", @"macos-rdp-daemon",
                        @"--force", path]
              timeout:60.0] != 0) {
        rdp_error("re-sign: codesign failed");
        return NO;
    }

    /* 3. Verify strictly. */
    if ([self runTask:@"/usr/bin/codesign"
                 args:@[@"--verify", @"--strict", path]
              timeout:30.0] != 0) {
        rdp_error("re-sign: codesign --verify --strict failed");
        return NO;
    }
    rdp_info("re-sign: codesigned + verified (identifier macos-rdp-daemon)");
    return YES;
}

/* ── NSTask helper (synchronous, with timeout) ────────────────────────── */
- (int)runTask:(NSString *)launchPath args:(NSArray<NSString *> *)args timeout:(NSTimeInterval)timeout {
    @try {
        NSTask *t = [[NSTask alloc] init];
        t.launchPath = launchPath;
        t.arguments  = args;
        t.standardOutput = [NSFileHandle fileHandleWithNullDevice];
        t.standardError  = [NSFileHandle fileHandleWithNullDevice];
        NSError *err = nil;
        if (![t launchAndReturnError:&err]) {
            rdp_error("runTask: failed to launch %s: %s",
                      launchPath.UTF8String, err.localizedDescription.UTF8String);
            return -1;
        }
        /* Bounded wait: poll for completion so a hung child can't wedge us. */
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
        while (t.isRunning && [deadline timeIntervalSinceNow] > 0)
            usleep(50 * 1000);
        if (t.isRunning) {
            rdp_error("runTask: %s timed out after %.0fs — terminating", launchPath.UTF8String, timeout);
            [t terminate];
            usleep(200 * 1000);
            if (t.isRunning) [t interrupt];
            return -2;
        }
        return (int)t.terminationStatus;
    } @catch (NSException *e) {
        rdp_error("runTask: exception running %s: %s", launchPath.UTF8String, e.reason.UTF8String);
        return -3;
    }
}

/* ── networking (synchronous via semaphore on self.queue) ─────────────── */
- (NSMutableURLRequest *)request:(NSString *)url accept:(NSString *)accept {
    NSURL *u = [NSURL URLWithString:url];
    if (!u) return nil;
    NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:u];
    [r setValue:@"macos-rdp-daemon-updater" forHTTPHeaderField:@"User-Agent"];
    if (accept) [r setValue:accept forHTTPHeaderField:@"Accept"];
    NSString *token = envStr("RDP_UPDATE_TOKEN", nil);
    if (token) [r setValue:[@"Bearer " stringByAppendingString:token]
         forHTTPHeaderField:@"Authorization"];
    return r;
}

- (NSDictionary *)getJSON:(NSString *)url {
    NSMutableURLRequest *r = [self request:url accept:@"application/vnd.github+json"];
    if (!r) return nil;
    __block NSData *data = nil; __block NSInteger code = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [self.session dataTaskWithRequest:r
        completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
            if (err) rdp_verbose("getJSON %s: %s", url.UTF8String, err.localizedDescription.UTF8String);
            if ([resp isKindOfClass:[NSHTTPURLResponse class]]) code = ((NSHTTPURLResponse *)resp).statusCode;
            data = d;
            dispatch_semaphore_signal(sem);
        }];
    [task resume];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (code != 200 || !data) {
        rdp_verbose("getJSON %s: http %ld", url.UTF8String, (long)code);
        return nil;
    }
    NSError *jerr = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jerr];
    if (jerr) { rdp_verbose("getJSON %s: parse error %s", url.UTF8String, jerr.localizedDescription.UTF8String); return nil; }
    return [obj isKindOfClass:[NSDictionary class]] ? obj : nil;
}

- (BOOL)downloadURL:(NSString *)url toPath:(NSString *)path {
    NSMutableURLRequest *r = [self request:url accept:@"application/octet-stream"];
    if (!r) return NO;
    __block NSURL *tmpLoc = nil; __block NSInteger code = 0; __block BOOL ok = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDownloadTask *task = [self.session downloadTaskWithRequest:r
        completionHandler:^(NSURL *location, NSURLResponse *resp, NSError *err) {
            if (err) rdp_verbose("download %s: %s", url.UTF8String, err.localizedDescription.UTF8String);
            if ([resp isKindOfClass:[NSHTTPURLResponse class]]) code = ((NSHTTPURLResponse *)resp).statusCode;
            if (location && code == 200) {
                /* Move out of the session's temp dir synchronously before signal. */
                [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
                NSError *mvErr = nil;
                ok = [[NSFileManager defaultManager] moveItemAtURL:location
                                                             toURL:[NSURL fileURLWithPath:path]
                                                             error:&mvErr];
                if (!ok) rdp_error("download: move into place failed: %s", mvErr.localizedDescription.UTF8String);
            }
            (void)tmpLoc;
            dispatch_semaphore_signal(sem);
        }];
    [task resume];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (code != 200) rdp_verbose("download %s: http %ld", url.UTF8String, (long)code);
    return ok;
}

- (NSString *)fetchSha256FromURL:(NSString *)url {
    NSMutableURLRequest *r = [self request:url accept:@"application/octet-stream"];
    if (!r) return nil;
    __block NSData *data = nil; __block NSInteger code = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [self.session dataTaskWithRequest:r
        completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
            if ([resp isKindOfClass:[NSHTTPURLResponse class]]) code = ((NSHTTPURLResponse *)resp).statusCode;
            data = d;
            dispatch_semaphore_signal(sem);
        }];
    [task resume];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (code != 200 || !data) return nil;
    NSString *s = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return [self firstSha256InString:s];
}

/* ── sha256 helpers ───────────────────────────────────────────────────── */
- (NSString *)firstSha256InString:(NSString *)s {
    if (![s isKindOfClass:[NSString class]]) return nil;
    /* A .sha256 file is typically "<64hex>  filename". Grab the first 64-hex run. */
    NSError *e = nil;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"[0-9a-fA-F]{64}"
                                                                       options:0 error:&e];
    if (!re) return nil;
    NSTextCheckingResult *m = [re firstMatchInString:s options:0 range:NSMakeRange(0, s.length)];
    if (!m) return nil;
    return [[s substringWithRange:m.range] lowercaseString];
}

- (NSString *)sha256OfFile:(NSString *)path {
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:NULL];
    if (!data) return nil;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", digest[i]];
    return hex;
}

@end
