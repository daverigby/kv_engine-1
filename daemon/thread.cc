/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "config.h"
#include "connection.h"
#include "connections.h"
#include "cookie.h"
#include "front_end_thread.h"
#include "log_macros.h"
#include "memcached.h"
#include "settings.h"
#include "stats.h"
#include "timing_histogram.h"
#include "tracing.h"

#include <memcached/openssl.h>
#include <nlohmann/json.hpp>
#include <phosphor/phosphor.h>
#include <platform/cb_malloc.h>
#include <platform/platform.h>
#include <platform/socket.h>
#include <platform/strerror.h>

#include <fcntl.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef WIN32
#include <netinet/tcp.h> // For TCP_NODELAY etc
#endif
#include <memory>
#include <queue>

extern std::atomic<bool> memcached_shutdown;

/* An item in the connection queue. */
struct ConnectionQueueItem {
    ConnectionQueueItem(SOCKET sock, in_port_t port)
        : sfd(sock),
          parent_port(port) {
        // empty
    }

    SOCKET sfd;
    in_port_t parent_port;
};

ConnectionQueue::~ConnectionQueue() {
    while (!connections.empty()) {
        safe_close(connections.front()->sfd);
        connections.pop();
    }
}

std::unique_ptr<ConnectionQueueItem> ConnectionQueue::pop() {
    std::lock_guard<std::mutex> guard(mutex);
    if (connections.empty()) {
        return nullptr;
    }
    std::unique_ptr<ConnectionQueueItem> ret(std::move(connections.front()));
    connections.pop();
    return ret;
}

void ConnectionQueue::push(std::unique_ptr<ConnectionQueueItem> item) {
    std::lock_guard<std::mutex> guard(mutex);
    connections.push(std::move(item));
}

static FrontEndThread dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static std::vector<FrontEndThread> threads;
std::vector<TimingHistogram> scheduler_info;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static size_t init_count = 0;
static cb_mutex_t init_lock;
static cb_cond_t init_cond;

static void thread_libevent_process(evutil_socket_t fd, short which, void *arg);

/*
 * Creates a worker thread.
 */
static void create_worker(void (*func)(void *), void *arg, cb_thread_t *id,
                          const char* name) {
    int ret;

    if ((ret = cb_create_named_thread(id, func, arg, 0, name)) != 0) {
        FATAL_ERROR(EXIT_FAILURE,
                    "Can't create thread {}: {}",
                    name,
                    cb_strerror());
    }
}

/****************************** LIBEVENT THREADS *****************************/

void iterate_all_connections(std::function<void(Connection&)> callback) {
    for (auto& thr : threads) {
        TRACE_LOCKGUARD_TIMED(thr.mutex,
                              "mutex",
                              "iterate_all_connections::threadLock",
                              SlowMutexThreshold);
        iterate_thread_connections(&thr, callback);
    }
}

static bool create_notification_pipe(FrontEndThread& me) {
    if (cb::net::socketpair(SOCKETPAIR_AF,
                            SOCK_STREAM,
                            0,
                            reinterpret_cast<SOCKET*>(me.notify)) ==
        SOCKET_ERROR) {
        LOG_WARNING("Can't create notify pipe: {}",
                    cb_strerror(cb::net::get_socket_error()));
        return false;
    }

    for (auto sock : me.notify) {
        int flags = 1;
        const auto* flag_ptr = reinterpret_cast<const void*>(&flags);
        cb::net::setsockopt(
                sock, IPPROTO_TCP, TCP_NODELAY, flag_ptr, sizeof(flags));
        cb::net::setsockopt(
                sock, SOL_SOCKET, SO_REUSEADDR, flag_ptr, sizeof(flags));

        if (evutil_make_socket_nonblocking(sock) == -1) {
            LOG_WARNING("Failed to enable non-blocking: {}",
                        cb_strerror(cb::net::get_socket_error()));
            return false;
        }
    }
    return true;
}

static void setup_dispatcher(struct event_base *main_base,
                             void (*dispatcher_callback)(evutil_socket_t, short, void *))
{
    dispatcher_thread.base = main_base;
	dispatcher_thread.thread_id = cb_thread_self();
        if (!create_notification_pipe(dispatcher_thread)) {
            FATAL_ERROR(EXIT_FAILURE, "Unable to create notification pipe");
    }

    /* Listen for notifications from other threads */
    if ((event_assign(&dispatcher_thread.notify_event,
                      dispatcher_thread.base,
                      dispatcher_thread.notify[0],
                      EV_READ | EV_PERSIST,
                      dispatcher_callback,
                      nullptr) == -1) ||
        (event_add(&dispatcher_thread.notify_event, 0) == -1)) {
        FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
    }
}

/*
 * Set up a thread's information.
 */
static void setup_thread(FrontEndThread& me) {
    me.base = event_base_new();

    if (!me.base) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate event base");
    }

    /* Listen for notifications from other threads */
    if ((event_assign(&me.notify_event,
                      me.base,
                      me.notify[0],
                      EV_READ | EV_PERSIST,
                      thread_libevent_process,
                      &me) == -1) ||
        (event_add(&me.notify_event, 0) == -1)) {
        FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
    }
}

/*
 * Worker thread: main event loop
 */
static void worker_libevent(void *arg) {
    FrontEndThread* me = reinterpret_cast<FrontEndThread*>(arg);

    /* Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    cb_mutex_enter(&init_lock);
    init_count++;
    cb_cond_signal(&init_cond);
    cb_mutex_exit(&init_lock);

    event_base_loop(me->base, 0);

    // Event loop exited; cleanup before thread exits.
    ERR_remove_state(0);
}

static void drain_notification_channel(evutil_socket_t fd)
{
    /* Every time we want to notify a thread, we send 1 byte to its
     * notification pipe. When the thread wakes up, it tries to drain
     * it's notification channel before executing any other events.
     * Other threads (listener and other background threads) may notify
     * this thread up to 512 times since the last time we checked the
     * notification pipe, before we'll start draining the it again.
     */

    ssize_t nread;
    // Using a small size for devnull will avoid blowing up the stack
    char devnull[512];

    while ((nread = cb::net::recv(fd, devnull, sizeof(devnull), 0)) ==
           (int)sizeof(devnull)) {
        /* empty */
    }

    if (nread == -1) {
        LOG_WARNING("Can't read from libevent pipe: {}",
                    cb_strerror(cb::net::get_socket_error()));
    }
}

static void dispatch_new_connections(FrontEndThread& me) {
    std::unique_ptr<ConnectionQueueItem> item;
    while ((item = me.new_conn_queue.pop()) != nullptr) {
        if (conn_new(item->sfd, item->parent_port, me.base, &me) == nullptr) {
            LOG_WARNING("Failed to dispatch event for socket {}",
                        long(item->sfd));
            safe_close(item->sfd);
        }
    }
}

/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(evutil_socket_t fd, short which, void *arg) {
    auto& me = *reinterpret_cast<FrontEndThread*>(arg);

    // Start by draining the notification channel before doing any work.
    // By doing so we know that we'll be notified again if someone
    // tries to notify us while we're doing the work below (so we don't have
    // to care about race conditions for stuff people try to notify us
    // about.
    drain_notification_channel(fd);

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. The listen thread should
        // be stopped immediately.
        if (is_listen_thread()) {
            LOG_INFO("Stopping listen thread (thread.cc)");
            event_base_loopbreak(me.base);
            return;
        }

        if (signal_idle_clients(&me, -1, false) == 0) {
            LOG_INFO("Stopping worker thread {}", me.index);
            event_base_loopbreak(me.base);
            return;
        }
    }

    dispatch_new_connections(me);

    FrontEndThread::PendingIoMap pending;
    {
        std::lock_guard<std::mutex> lock(me.pending_io.mutex);
        me.pending_io.map.swap(pending);
    }

    TRACE_LOCKGUARD_TIMED(me.mutex,
                          "mutex",
                          "thread_libevent_process::threadLock",
                          SlowMutexThreshold);

    for (auto io : pending) {
        auto* c = io.first;
        auto status = io.second;
        if (c->getSocketDescriptor() != INVALID_SOCKET &&
            !c->isRegisteredInLibevent()) {
            /* The socket may have been shut down while we're looping */
            /* in delayed shutdown */
            c->registerEvent();
        }

        // @todo we need to refactor this so we keep the _cookies_ and not
        //       the connections
        c->getCookieObject().setAiostat(status);
        c->getCookieObject().setEwouldblock(false);
        /*
         * We don't want the thread to keep on serving all of the data
         * from the context of the notification pipe, so just let it
         * run one time to set up the correct mask in libevent
         */
        c->setNumEvents(1);
        run_event_loop(c, EV_READ | EV_WRITE);
    }

    /*
     * I could look at all of the connection objects bound to dying buckets
     */
    if (me.deleting_buckets) {
        notify_thread_bucket_deletion(me);
    }

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. If we don't have
        // any connections bound to this thread we can just shut down
        auto connected = signal_idle_clients(&me, -1, true);
        if (connected == 0) {
            LOG_INFO("Stopping worker thread {}", me.index);
            event_base_loopbreak(me.base);
        } else {
            // @todo Change loglevel once MB-16255 is resolved
            LOG_INFO("Waiting for {} connected clients on worker thread {}",
                     connected,
                     me.index);
        }
    }
}

void notify_io_complete(gsl::not_null<const void*> void_cookie,
                        ENGINE_ERROR_CODE status) {
    auto* ccookie = reinterpret_cast<const Cookie*>(void_cookie.get());
    auto& cookie = const_cast<Cookie&>(*ccookie);

    auto* thr = cookie.getConnection().getThread();
    if (thr == nullptr) {
        auto json = cookie.getConnection().toJSON().dump();
        LOG_ERROR(
                "notify_io_complete: got a notification on a cookie which "
                "isn't bound to a thread: {}",
                json);
        throw std::runtime_error(
                "notify_io_complete: connection should be bound to a thread: " +
                json);
    }

    LOG_DEBUG("notify_io_complete: Got notify from {}, status {}",
              cookie.getConnection().getId(),
              status);

    /* kick the thread in the butt */
    if (add_conn_to_pending_io_list(&cookie.getConnection(), status)) {
        notify_thread(*thr);
    }
}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, or because of an incoming connection.
 */
void dispatch_conn_new(SOCKET sfd, in_port_t parent_port) {
    size_t tid = (last_thread + 1) % settings.getNumWorkerThreads();
    auto& thread = threads[tid];
    last_thread = gsl::narrow<int>(tid);

    try {
        std::unique_ptr<ConnectionQueueItem> item(
            new ConnectionQueueItem(sfd, parent_port));
        thread.new_conn_queue.push(std::move(item));
    } catch (const std::bad_alloc& e) {
        LOG_WARNING("dispatch_conn_new: Failed to dispatch new connection: {}",
                    e.what());
        safe_close(sfd);
        return ;
    }

    notify_thread(thread);
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread() {
    return dispatcher_thread.thread_id == cb_thread_self();
}

void notify_dispatcher() {
    notify_thread(dispatcher_thread);
}

/******************************* GLOBAL STATS ******************************/

void threadlocal_stats_reset(std::vector<thread_stats>& thread_stats) {
    for (auto& ii : thread_stats) {
        ii.reset();
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(size_t nthr,
                 struct event_base* main_base,
                 void (*dispatcher_callback)(evutil_socket_t, short, void*)) {
    cb_mutex_initialize(&init_lock);
    cb_cond_initialize(&init_cond);

    scheduler_info.resize(nthr);

    try {
        threads = std::vector<FrontEndThread>(nthr);
    } catch (const std::bad_alloc&) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate thread descriptors");
    }

    setup_dispatcher(main_base, dispatcher_callback);

    for (size_t ii = 0; ii < nthr; ii++) {
        if (!create_notification_pipe(threads[ii])) {
            FATAL_ERROR(EXIT_FAILURE, "Cannot create notification pipe");
        }
        threads[ii].index = ii;

        setup_thread(threads[ii]);
    }

    /* Create threads after we've done all the libevent setup. */
    for (auto& thread : threads) {
        const std::string name = "mc:worker_" + std::to_string(thread.index);
        create_worker(
                worker_libevent, &thread, &thread.thread_id, name.c_str());
    }

    /* Wait for all the threads to set themselves up before returning. */
    cb_mutex_enter(&init_lock);
    while (init_count < nthr) {
        cb_cond_wait(&init_cond, &init_lock);
    }
    cb_mutex_exit(&init_lock);
}

void threads_shutdown() {
    for (auto& thread : threads) {
        notify_thread(thread);
        cb_join_thread(thread.thread_id);
    }
}

void threads_cleanup() {
    for (auto& thread : threads) {
        event_base_free(thread.base);
    }
}

FrontEndThread::~FrontEndThread() {
    for (auto& sock : notify) {
        if (sock != INVALID_SOCKET) {
            safe_close(sock);
        }
    }
}

void threads_notify_bucket_deletion() {
    for (auto& thr : threads) {
        notify_thread(thr);
    }
}

void threads_complete_bucket_deletion() {
    for (auto& thr : threads) {
        TRACE_LOCKGUARD_TIMED(thr.mutex,
                              "mutex",
                              "threads_complete_bucket_deletion::threadLock",
                              SlowMutexThreshold);
        thr.deleting_buckets--;
    }
}

void threads_initiate_bucket_deletion() {
    for (auto& thr : threads) {
        TRACE_LOCKGUARD_TIMED(thr.mutex,
                              "mutex",
                              "threads_initiate_bucket_deletion::threadLock",
                              SlowMutexThreshold);
        thr.deleting_buckets++;
    }
}

void notify_thread(FrontEndThread& thread) {
    if (cb::net::send(thread.notify[1], "", 1, 0) != 1 &&
        !cb::net::is_blocking(cb::net::get_socket_error())) {
        LOG_WARNING("Failed to notify thread: {}",
                    cb_strerror(cb::net::get_socket_error()));
    }
}

int add_conn_to_pending_io_list(Connection* c, ENGINE_ERROR_CODE status) {
    auto* thread = c->getThread();

    std::pair<FrontEndThread::PendingIoMap::iterator, bool> result;
    {
        std::lock_guard<std::mutex> lock(thread->pending_io.mutex);
        result = thread->pending_io.map.emplace(c, status);
    }
    int notify = result.second ? 1 : 0;
    return notify;
}
