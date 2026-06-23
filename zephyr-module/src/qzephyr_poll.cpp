#include <poll.h>
#include <time.h>

extern "C++" {
int qt_poll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout_ts)
{
    int timeout_ms = -1;
    if (timeout_ts) {
        timeout_ms = static_cast<int>(timeout_ts->tv_sec * 1000
                                      + timeout_ts->tv_nsec / 1000000);
    }
    return ::poll(fds, static_cast<int>(nfds), timeout_ms);
}
}
