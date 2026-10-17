#include "transport.h"

#import <Foundation/Foundation.h>

#include <cstdio>

// Writes a data task's bytes to the file as they arrive, and signals when the task has ended.
@interface ASRDFetchDelegate : NSObject <NSURLSessionDataDelegate> {
@public
    std::string path;
    int64_t offset;                                  // what the file holds before this fetch; 0 to start over
    int64_t bytes;                                   // what it holds now
    const std::function<void(int64_t)> * progress;
    std::string error;
    FILE * file;
    dispatch_semaphore_t done;
}
@end

@implementation ASRDFetchDelegate

- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    const NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]]
                                 ? static_cast<NSHTTPURLResponse *>(response).statusCode
                                 : 0;
    if (status != 200 && status != 206) {
        error = "the server answered " + std::to_string(status);
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    // 200 to a range request: the whole file comes again.
    if (status == 200) offset = 0;
    file = std::fopen(path.c_str(), offset > 0 ? "ab" : "wb");
    if (!file) {
        error = "cannot write " + path;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    bytes = offset;
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    __block bool written = true;
    [data enumerateByteRangesUsingBlock:^(const void * chunk, NSRange range, BOOL * stop) {
        if (std::fwrite(chunk, 1, range.length, file) != range.length) {
            written = false;
            *stop = YES;
        }
    }];
    if (!written) {
        error = "cannot write " + path + "; is the disk full?";
        [task cancel];
        return;
    }
    bytes += static_cast<int64_t>(data.length);
    (*progress)(bytes);
}

// Hugging Face redirects to its CDN; the redirected request asks for the same range.
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
    NSMutableURLRequest * next = [request mutableCopy];
    if (offset > 0) [next setValue:[NSString stringWithFormat:@"bytes=%lld-", offset] forHTTPHeaderField:@"Range"];
    completionHandler(next);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)failure {
    if (failure && error.empty() &&
        !([failure.domain isEqualToString:NSURLErrorDomain] && failure.code == NSURLErrorCancelled))
        error = failure.localizedDescription.UTF8String;
    if (file) {
        if (std::fclose(file) != 0 && error.empty()) error = "cannot write " + path;
        file = nullptr;
    }
    dispatch_semaphore_signal(done);
}

@end

namespace asrd {
namespace {

class UrlSessionTransport : public Transport {
public:
    bool fetch(const std::string & url, const std::string & dest, int64_t offset,
               const std::function<void(int64_t)> & progress, Cancellation & cancel, std::string & error) override {
        @autoreleasepool {
            ASRDFetchDelegate * delegate = [ASRDFetchDelegate new];
            delegate->path = dest;
            delegate->offset = offset;
            delegate->bytes = offset;
            delegate->progress = &progress;
            delegate->file = nullptr;
            delegate->done = dispatch_semaphore_create(0);

            NSURLSessionConfiguration * config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = 60;
            NSOperationQueue * queue = [NSOperationQueue new];
            queue.maxConcurrentOperationCount = 1;
            NSURLSession * session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];

            NSURL * address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (!address) {
                error = "not a URL: " + url;
                [session invalidateAndCancel];
                return false;
            }
            NSMutableURLRequest * request = [NSMutableURLRequest requestWithURL:address];
            if (offset > 0)
                [request setValue:[NSString stringWithFormat:@"bytes=%lld-", offset] forHTTPHeaderField:@"Range"];
            NSURLSessionDataTask * task = [session dataTaskWithRequest:request];
            if (!cancel.arm([task] { [task cancel]; })) {
                [session invalidateAndCancel];
                return false;
            }
            [task resume];
            dispatch_semaphore_wait(delegate->done, DISPATCH_TIME_FOREVER);
            cancel.disarm();
            // The session holds its delegate until it is invalidated.
            [session finishTasksAndInvalidate];
            if (cancel.cancelled()) return false;
            if (!delegate->error.empty()) {
                error = delegate->error;
                return false;
            }
            return true;
        }
    }
};

}  // namespace

std::unique_ptr<Transport> make_url_session_transport() {
    return std::make_unique<UrlSessionTransport>();
}

}  // namespace asrd
