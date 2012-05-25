#include "networking.h"
#define EV_STANDALONE 1
#define EV_API_STATIC 1
#define EV_COMPAT3 0
#define EV_MULTIPLICITY 0
#ifdef __linux__
#define EV_USE_CLOCK_SYSCALL 0
#define EV_USE_EPOLL 1
#endif
#ifdef __MACH__
#define EV_USE_KQUEUE 1
#endif
#include "ev.c"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include "conn_handler.h"


/**
 * Default listen backlog size for
 * our TCP listener.
 */
#define BACKLOG_SIZE 64

/**
 * How big should the default connection
 * buffer size be. One page seems reasonable
 * since most requests will not be this large
 */
#define INIT_CONN_BUF_SIZE 32768

/**
 * This is the scale factor we use when
 * we are growing our connection buffers.
 * We want this to be aggressive enough to reduce
 * the number of resizes, but to also avoid wasted
 * space. With this, we will go from:
 * 32K -> 256K -> 2MB -> 16MB
 */
#define CONN_BUF_MULTIPLIER 8


/**
 * Stores the thread specific user data.
 */
typedef struct {
    statsite_networking *netconf;
    ev_io *watcher;
    int ready_events;
} worker_ev_userdata;

/**
 * Represents a simple circular buffer
 */
typedef struct {
    int write_cursor;
    int read_cursor;
    uint32_t buf_size;
    char *buffer;
} circular_buffer;

/**
 * Stores the connection specific data.
 * We initialize one of these per connection
 */
struct conn_info {
    volatile int ref_count;

    statsite_networking *netconf;
    ev_io client;
    volatile int should_schedule;
    circular_buffer input;

    /*
     * Output is handled in a special way.
     * If use_write_buf is off, then we make
     * the writes directly, otherwise we need to
     * write to our circular buffer. Once the buffer
     * is depleted, we switch use_write_buf back off,
     * and go back to writing directly.
     *
     * The logic is that most clients have a quick
     * check/set command pair which fits in the TCP
     * buffers. Some bulk operations with tons of checks
     * or sets may overwhelm our buffers however. This
     * allows us to minimize copies and latency for most
     * clients, while still supporting the massive bulk
     * loads.
     */
    volatile int use_write_buf;
    ev_io write_client;
    circular_buffer output;
};
typedef struct conn_info conn_info;


/**
 * Represents the various types
 * of async events we could be
 * processing.
 */
typedef enum {
    EXIT,               // ev_break should be invoked
    SCHEDULE_WATCHER,   // watcher should be started
} ASYNC_EVENT_TYPE;

/**
 * Structure used to store async events
 * that need to processed when we trigger
 * the loop_async watcher.
 */
struct async_event {
    ASYNC_EVENT_TYPE event_type;
    ev_io *watcher;
    struct async_event *next;
};
typedef struct async_event async_event;

/**
 * Defines a structure that is
 * used to store the state of the networking
 * stack.
 */
struct statsite_networking {
    volatile int should_run;  // Should the workers continue to run
    statsite_config *config;

    ev_io tcp_client;
    ev_io udp_client;

    ev_async loop_async;            // Allows async interrupts
    volatile async_event *events;   // List of pending events

    ev_timer flush_timer;

    pthread_t thread;       // Thread references
};


// Static typedefs
static void schedule_async(statsite_networking *netconf,
                            ASYNC_EVENT_TYPE event_type,
                            ev_io *watcher);
static void prepare_event(ev_io *watcher, int revents);
static void handle_async_event(ev_async *watcher, int revents);
static void handle_flush_event(ev_timer *watcher, int revents);
static void handle_new_client(int listen_fd, worker_ev_userdata* data);
static int handle_client_data(ev_io *watch, worker_ev_userdata* data);
static int handle_udp_message(ev_io *watch, worker_ev_userdata* data);
static void invoke_event_handler(worker_ev_userdata* data);

// Utility methods
static int set_client_sockopts(int client_fd);
static conn_info* get_conn(statsite_networking *netconf);
static int send_client_response_buffered(conn_info *conn, char **response_buffers, int *buf_sizes, int num_bufs);
static int send_client_response_direct(conn_info *conn, char **response_buffers, int *buf_sizes, int num_bufs);

static void incref_client_connection(conn_info *conn);
static void decref_client_connection(conn_info *conn);
static void private_close_connection(conn_info *conn);

// Circular buffer method
static void circbuf_init(circular_buffer *buf);
static void circbuf_clear(circular_buffer *buf);
static void circbuf_free(circular_buffer *buf);
static uint64_t circbuf_avail_buf(circular_buffer *buf);
static void circbuf_grow_buf(circular_buffer *buf);
static void circbuf_setup_readv_iovec(circular_buffer *buf, struct iovec *vectors, int *num_vectors);
static void circbuf_setup_writev_iovec(circular_buffer *buf, struct iovec *vectors, int *num_vectors);
static void circbuf_advance_write(circular_buffer *buf, uint64_t bytes);
static void circbuf_advance_read(circular_buffer *buf, uint64_t bytes);
static int circbuf_write(circular_buffer *buf, char *in, uint64_t bytes);

/**
 * Initializes the TCP listener
 * @arg netconf The network configuration
 * @return 0 on success.
 */
static int setup_tcp_listener(statsite_networking *netconf) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = PF_INET;
    addr.sin_port = htons(netconf->config->tcp_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Make the socket, bind and listen
    int tcp_listener_fd = socket(PF_INET, SOCK_STREAM, 0);
    int optval = 1;
    if (setsockopt(tcp_listener_fd, SOL_SOCKET,
                SO_REUSEADDR, &optval, sizeof(optval))) {
        syslog(LOG_ERR, "Failed to set SO_REUSEADDR! Err: %s", strerror(errno));
        close(tcp_listener_fd);
        return 1;
    }
    if (bind(tcp_listener_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        syslog(LOG_ERR, "Failed to bind on TCP socket! Err: %s", strerror(errno));
        close(tcp_listener_fd);
        return 1;
    }
    if (listen(tcp_listener_fd, BACKLOG_SIZE) != 0) {
        syslog(LOG_ERR, "Failed to listen on TCP socket! Err: %s", strerror(errno));
        close(tcp_listener_fd);
        return 1;
    }

    // Create the libev objects
    ev_io_init(&netconf->tcp_client, prepare_event,
                tcp_listener_fd, EV_READ);
    ev_io_start(&netconf->tcp_client);
    return 0;
}

/**
 * Initializes the UDP Listener.
 * @arg netconf The network configuration
 * @return 0 on success.
 */
static int setup_udp_listener(statsite_networking *netconf) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = PF_INET;
    addr.sin_port = htons(netconf->config->udp_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Make the socket, bind and listen
    int udp_listener_fd = socket(PF_INET, SOCK_DGRAM, 0);
    int optval = 1;
    if (setsockopt(udp_listener_fd, SOL_SOCKET,
                SO_REUSEADDR, &optval, sizeof(optval))) {
        syslog(LOG_ERR, "Failed to set SO_REUSEADDR! Err: %s", strerror(errno));
        close(udp_listener_fd);
        return 1;
    }
    if (bind(udp_listener_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        syslog(LOG_ERR, "Failed to bind on UDP socket! Err: %s", strerror(errno));
        close(udp_listener_fd);
        return 1;
    }

    // Allocate a connection object for the UDP socket,
    // ensure a min-buffer size of 64K
    conn_info *conn = get_conn(netconf);
    while (circbuf_avail_buf(&conn->input) < 65536) {
        circbuf_grow_buf(&conn->input);
    }
    netconf->udp_client.data = conn;

    // Create the libev objects
    ev_io_init(&netconf->udp_client, prepare_event,
                udp_listener_fd, EV_READ);
    ev_io_start(&netconf->udp_client);
    return 0;
}

/**
 * Initializes the networking interfaces
 * @arg config Takes the bloom server configuration
 * @arg mgr The filter manager to pass up to the connection handlers
 * @arg netconf Output. The configuration for the networking stack.
 */
int init_networking(statsite_config *config, statsite_networking **netconf_out) {
    // Make the netconf structure
    statsite_networking *netconf = calloc(1, sizeof(struct statsite_networking));

    // Initialize
    netconf->events = NULL;
    netconf->config = config;
    netconf->should_run = 1;
    netconf->thread = NULL;

    /**
     * Check if we can use kqueue instead of select.
     * By default, libev will not use kqueue since it only
     * works for sockets, which is all we need.
     */
    int ev_mode = EVFLAG_AUTO;
    if (ev_supported_backends () & ~ev_recommended_backends () & EVBACKEND_KQUEUE) {
        ev_mode = EVBACKEND_KQUEUE;
    }

    if (!ev_default_loop (ev_mode)) {
        syslog(LOG_CRIT, "Failed to initialize libev!");
        free(netconf);
        return 1;
    }

    // Setup the TCP listener
    int res = setup_tcp_listener(netconf);
    if (res != 0) {
        free(netconf);
        return 1;
    }

    // Setup the UDP listener
    res = setup_udp_listener(netconf);
    if (res != 0) {
        ev_io_stop(&netconf->tcp_client);
        close(netconf->tcp_client.fd);
        free(netconf);
        return 1;
    }

    // Setup the async handler
    ev_async_init(&netconf->loop_async, handle_async_event);
    ev_async_start(&netconf->loop_async);

    // Setup the timer
    ev_timer_init(&netconf->flush_timer, handle_flush_event, config->flush_interval, config->flush_interval);
    ev_timer_start(&netconf->flush_timer);

    // Prepare the conn handlers
    init_conn_handler(config);

    // Success!
    *netconf_out = netconf;
    return 0;
}


/**
 * Called to schedule an async event. Mostly a convenience
 * method to wrap some of the logic.
 */
static void schedule_async(statsite_networking *netconf,
                            ASYNC_EVENT_TYPE event_type,
                            ev_io *watcher) {
    // Make a new async event
    async_event *event = malloc(sizeof(async_event));

    // Initialize
    event->event_type = event_type;
    event->watcher = watcher;

    // Set the next pointer, and add us to the head
    event->next = (async_event*)netconf->events;
    netconf->events = event;

    // Send to our async watcher
    ev_async_send(&netconf->loop_async);
}

/**
 * Called when an event is ready to be processed by libev.
 * We need to do _very_ little work here. Basically just
 * setup the userdata to process the event and return.
 */
static void prepare_event(ev_io *watcher, int revents) {
    // Get the user data
    worker_ev_userdata *data = ev_userdata();

    // Set everything if we don't have a watcher
    if (!data->watcher) {
        data->watcher = watcher;
        data->ready_events = revents;

        // Stop listening for now
        ev_io_stop(watcher);
    }
}


/**
 * Called when a message is sent to netconf->loop_async.
 * This is usually to signal that some internal control
 * flow related to the event loop needs to take place.
 * For example, we might need to re-enable some ev_io* watchers,
 * or exit the loop.
 */
static void handle_async_event(ev_async *watcher, int revents) {
    // Get the user data
    worker_ev_userdata *data = ev_userdata();

    // Get a reference to the head, set the head to NULL
    async_event *event = (async_event*)data->netconf->events;
    data->netconf->events = NULL;

    async_event *next;
    while (event) {
        // Handle based on the event
        switch (event->event_type) {
            case EXIT:
                ev_break(EVBREAK_ALL);
                break;

            case SCHEDULE_WATCHER:
                ev_io_start(event->watcher);
                break;

            default:
                syslog(LOG_ERR, "Unknown async event type!");
                break;
        }

        // Grab the next event, free this one, and repeat
        next = event->next;
        free(event);
        event = next;
    }
}


/**
 * Invoked when our flush timer is reached.
 * We need to instruct the connection handler about this.
 */
static void handle_flush_event(ev_timer *watcher, int revents) {
    // Inform the connection handler of the timeout
    flush_interval_trigger();
}

/**
 * Invoked when a TCP listening socket fd is ready
 * to accept a new client. Accepts the client, initializes
 * the connection buffers, and prepares to start listening
 * for client data
 */
static void handle_new_client(int listen_fd, worker_ev_userdata* data) {
    // Accept the client connection
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd,
                        (struct sockaddr*)&client_addr,
                        &client_addr_len);

    // Check for an error
    if (client_fd == -1) {
        syslog(LOG_ERR, "Failed to accept() connection! %s.", strerror(errno));
        return;
    }

    // Setup the socket
    if (set_client_sockopts(client_fd)) {
        return;
    }

    // Debug info
    syslog(LOG_DEBUG, "Accepted client connection: %s %d [%d]",
            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);

    // Get the associated conn object
    conn_info *conn = get_conn(data->netconf);

    // Initialize the libev stuff
    ev_io_init(&conn->client, prepare_event, client_fd, EV_READ);
    ev_io_init(&conn->write_client, prepare_event, client_fd, EV_WRITE);

    // Schedule the new client
    schedule_async(data->netconf, SCHEDULE_WATCHER, &conn->client);
}


/**
 * Invoked when a client connection has data ready to be read.
 * We need to take care to add the data to our buffers, and then
 * invoke the connection handlers who have the business logic
 * of what to do.
 */
static int handle_client_data(ev_io *watch, worker_ev_userdata* data) {
    // Get the associated connection struct
    conn_info *conn = watch->data;

    /**
     * Figure out how much space we have to write.
     * If we have < 50% free, we resize the buffer using
     * a multiplier.
     */
    int avail_buf = circbuf_avail_buf(&conn->input);
    if (avail_buf < conn->input.buf_size / 2) {
        circbuf_grow_buf(&conn->input);
    }

    // Build the IO vectors to perform the read
    struct iovec vectors[2];
    int num_vectors;
    circbuf_setup_readv_iovec(&conn->input, (struct iovec*)&vectors, &num_vectors);

    // Issue the read
    ssize_t read_bytes = readv(watch->fd, (struct iovec*)&vectors, num_vectors);

    // Make sure we actually read something
    if (read_bytes == 0) {
        syslog(LOG_DEBUG, "Closed client connection. [%d]\n", conn->client.fd);
        close_client_connection(conn);
        return 1;
    } else if (read_bytes == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            syslog(LOG_ERR, "Failed to read() from connection [%d]! %s.",
                    conn->client.fd, strerror(errno));
            close_client_connection(conn);
        }
        return 1;
    }

    // Update the write cursor
    circbuf_advance_write(&conn->input, read_bytes);
    return 0;
}


/**
 * Invoked when a UDP connection has a message ready to be read.
 * We need to take care to add the data to our buffers, and then
 * invoke the connection handlers who have the business logic
 * of what to do.
 */
static int handle_udp_message(ev_io *watch, worker_ev_userdata* data) {
    // Get the associated connection struct
    conn_info *conn = watch->data;

    // Clear the input buffer
    circbuf_clear(&conn->input);

    // Build the IO vectors to perform the read
    struct iovec vectors[2];
    int num_vectors;
    circbuf_setup_readv_iovec(&conn->input, (struct iovec*)&vectors, &num_vectors);

    /*
     * Issue the read, allows use the first vector.
     * since we just cleared the buffer.
     */
    ssize_t read_bytes = recv(watch->fd, vectors[0].iov_base,
                                vectors[0].iov_len, 0);

    // Make sure we actually read something
    if (read_bytes == 0) {
        syslog(LOG_DEBUG, "Got empty UDP packet. [%d]\n", watch->fd);
        return 1;
    } else if (read_bytes == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            syslog(LOG_ERR, "Failed to recv() from connection [%d]! %s.",
                    watch->fd, strerror(errno));
        }
        return 1;
    }

    // Update the write cursor
    circbuf_advance_write(&conn->input, read_bytes);
    return 0;
}


/**
 * Invoked when a client connection is ready to be written to.
 */
static int handle_client_writebuf(ev_io *watch, worker_ev_userdata* data) {
    // Get the associated connection struct
    conn_info *conn = watch->data;

    // Build the IO vectors to perform the write
    struct iovec vectors[2];
    int num_vectors;
    circbuf_setup_writev_iovec(&conn->output, (struct iovec*)&vectors, &num_vectors);

    // Issue the write
    ssize_t write_bytes = writev(watch->fd, (struct iovec*)&vectors, num_vectors);

    int reschedule = 0;
    if (write_bytes > 0) {
        // Update the cursor
        circbuf_advance_read(&conn->output, write_bytes);

        // Check if we should reset the use_write_buf.
        // This is done when the buffer size is 0.
        if (conn->output.read_cursor == conn->output.write_cursor) {
            conn->use_write_buf = 0;
        } else {
            reschedule = 1;
        }
    }

    // Handle any errors
    if (write_bytes <= 0 && (errno != EAGAIN && errno != EINTR)) {
        syslog(LOG_ERR, "Failed to write() to connection [%d]! %s.",
                conn->client.fd, strerror(errno));
        close_client_connection(conn);
        decref_client_connection(conn);
        return 1;
    }

    // Check if we should reschedule or end
    if (reschedule) {
        schedule_async(data->netconf, SCHEDULE_WATCHER, &conn->write_client);
    } else {
        decref_client_connection(conn);
    }
    return 0;
}

/**
 * Reads the thread specific userdata to figure out what
 * we need to handle. Things that purely effect the network
 * stack should be handled here, but otherwise we should defer
 * to the connection handlers.
 */
static void invoke_event_handler(worker_ev_userdata* data) {
    // Get the offending handle
    ev_io *watcher = data->watcher;
    int fd = watcher->fd;

    // Check if this is either of the listeners
    if (watcher == &data->netconf->tcp_client) {
        // Accept the new client
        handle_new_client(fd, data);

        // Reschedule the listener
        schedule_async(data->netconf, SCHEDULE_WATCHER, watcher);
        return;

    // If it is write ready, dispatch the write handler
    }  else if (data->ready_events & EV_WRITE) {
        handle_client_writebuf(watcher, data);
        return;
    }

    // Check for UDP inbound
    if (watcher == &data->netconf->udp_client) {
        // Read the message and process
        if (!handle_udp_message(watcher, data)) {
            statsite_conn_handler handle = {data->netconf->config, watcher->data};
            handle_client_connect(&handle);
        }

        // Reschedule the listener
        schedule_async(data->netconf, SCHEDULE_WATCHER, watcher);
        return;
    }

    /*
     * If it is not a listener, it must be a connected
     * client. We should just read all the available data,
     * append it to the buffers, and then invoke the
     * connection handlers.
     */
    conn_info *conn = watcher->data;
    incref_client_connection(conn);

    if (!handle_client_data(watcher, data)) {
        statsite_conn_handler handle = {data->netconf->config, watcher->data};
        handle_client_connect(&handle);
    }

    // Reschedule the watcher, unless told otherwise.
    if (conn->should_schedule) {
        schedule_async(data->netconf, SCHEDULE_WATCHER, watcher);
    }
    decref_client_connection(conn);
}


/**
 * Entry point for threads to join the networking
 * stack. This method blocks indefinitely until the
 * network stack is shutdown.
 * @arg netconf The configuration for the networking stack.
 */
void start_networking_worker(statsite_networking *netconf) {
    // Allocate our user data
    worker_ev_userdata data;
    data.netconf = netconf;
    netconf->thread = pthread_self();

    // Set the user data to be for this thread
    ev_set_userdata(&data);

    // Run forever until we are told to halt
    while (netconf->should_run) {
        data.watcher = NULL;
        data.ready_events = 0;

        // Run one iteration of the event loop
        ev_run(EVRUN_ONCE);

        // Process the event
        if (data.watcher) {
            invoke_event_handler(&data);
        }
    }
    return;
}

/**
 * Shuts down all the connections
 * and listeners and prepares to exit.
 * @arg netconf The config for the networking stack.
 */
int shutdown_networking(statsite_networking *netconf) {
    // Instruct the threads to shutdown
    netconf->should_run = 0;

    // Break the EV loop
    schedule_async(netconf, EXIT, NULL);

    // Wait for the thread to return
    if (netconf->thread) pthread_join(netconf->thread, NULL);

    // Stop listening for new connections
    ev_io_stop(&netconf->tcp_client);
    close(netconf->tcp_client.fd);
    ev_io_stop(&netconf->udp_client);
    close(netconf->udp_client.fd);

    // Stop the other timers
    ev_async_stop(&netconf->loop_async);
    ev_timer_stop(&netconf->flush_timer);

    // TODO: Close all the client connections
    // ??? For now, we just leak the memory
    // since we are shutdown down anyways...

    // Free the event loop
    ev_loop_destroy(EV_DEFAULT);

    // Free the netconf
    free(netconf);
    return 0;
}

/*
 * These are externally visible methods for
 * interacting with the connection buffers.
 */

/**
 * Increases the reference count of the
 * connection info object.
 */
static void incref_client_connection(conn_info *conn) {
    // Atomic decrement
    conn->ref_count++;
}

static void decref_client_connection(conn_info *conn) {
    // Atomic decrement
    int refs = --conn->ref_count;
    if (refs == 0) private_close_connection(conn);
}

/**
 * Called to close and cleanup a client connection.
 * Must be called when the connection is not already
 * scheduled. e.g. After ev_io_stop() has been called.
 * Leaves the connection in the conns list so that it
 * can be re-used.
 * @arg conn The connection to close
 */
void close_client_connection(conn_info *conn) {
    // Stop scheduling
    conn->should_schedule = 0;

    // Atomic decrement
    int refs = --conn->ref_count;

    // If our refcount is still non-zero, do nothing.
    if (refs != 0) {
        return;
    }

    // Close the connection
    private_close_connection(conn);
}

static void private_close_connection(conn_info *conn) {
    // Stop the libev clients
    ev_io_stop(&conn->client);
    ev_io_stop(&conn->write_client);

    // Clear everything out
    circbuf_free(&conn->input);
    circbuf_free(&conn->output);

    // Close the fd
    syslog(LOG_DEBUG, "Closed connection. [%d]", conn->client.fd);
    close(conn->client.fd);
    free(conn);
}


/**
 * Sends a response to a client.
 * @arg conn The client connection
 * @arg response_buffers A list of response buffers to send
 * @arg buf_sizes A list of the buffer sizes
 * @arg num_bufs The number of response buffers
 * @return 0 on success.
 */
int send_client_response(conn_info *conn, char **response_buffers, int *buf_sizes, int num_bufs) {
    // Bail if we shouldn't schedule
    if (!conn->should_schedule) return 0;

    // Check if we are doing buffered writes
    if (conn->use_write_buf) {
        return send_client_response_buffered(conn, response_buffers, buf_sizes, num_bufs);
    } else {
        return send_client_response_direct(conn, response_buffers, buf_sizes, num_bufs);
    }
}


static int send_client_response_buffered(conn_info *conn, char **response_buffers, int *buf_sizes, int num_bufs) {
    // Might not be using buffered writes anymore
    if (!conn->use_write_buf) {
        return send_client_response_direct(conn, response_buffers, buf_sizes, num_bufs);
    }

    // Copy the buffers to the output buffer
    int res = 0;
    for (int i=0; i< num_bufs; i++) {
        res = circbuf_write(&conn->output, response_buffers[i], buf_sizes[i]);
        if (res) break;
    }
    return res;
}


static int send_client_response_direct(conn_info *conn, char **response_buffers, int *buf_sizes, int num_bufs) {
    // Stack allocate the iovectors
    struct iovec *vectors = alloca(num_bufs * sizeof(struct iovec));

    // Setup all the pointers
    ssize_t total_bytes = 0;
    for (int i=0; i < num_bufs; i++) {
        vectors[i].iov_base = response_buffers[i];
        vectors[i].iov_len = buf_sizes[i];
        total_bytes += buf_sizes[i];
    }

    // Perform the write
    ssize_t sent = writev(conn->client.fd, vectors, num_bufs);
    if (sent == total_bytes) return 0;

    // Check for a fatal error
    if (sent == -1) {
        if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) {
            syslog(LOG_ERR, "Failed to send() to connection [%d]! %s.",
                    conn->client.fd, strerror(errno));
            close_client_connection(conn);
            return 1;
        }
    }

    // Figure out which buffer we left off on
    int skip_bytes = 0;
    int index = 0;
    for (index; index < num_bufs; index++) {
        skip_bytes += buf_sizes[index];
        if (skip_bytes > sent) {
            skip_bytes -= buf_sizes[index];
            break;
        }
    }

    // Copy the buffers
    int res, offset;
    for (int i=index; i < num_bufs; i++) {
        offset = 0;
        if (i == index && skip_bytes < sent) {
            offset = sent - skip_bytes;
        }
        res = circbuf_write(&conn->output, response_buffers[i] + offset, buf_sizes[i] - offset);
        if (res) return 1;
    }

    // Setup the async write
    conn->use_write_buf = 1;
    incref_client_connection(conn);
    schedule_async(conn->netconf, SCHEDULE_WATCHER, &conn->write_client);

    // Done
    return 0;
}


/**
 * This method is used to conveniently extract commands from the
 * command buffer. It scans up to a terminator, and then sets the
 * buf to the start of the buffer, and buf_len to the length
 * of the buffer. The output param should_free indicates that
 * the caller should free the buffer pointed to by buf when it is finished.
 * This method consumes the bytes from the underlying buffer, freeing
 * space for later reads.
 * @arg conn The client connection
 * @arg terminator The terminator charactor to look for. Replaced by null terminator.
 * @arg buf Output parameter, sets the start of the buffer.
 * @arg buf_len Output parameter, the length of the buffer.
 * @arg should_free Output parameter, should the buffer be freed by the caller.
 * @return 0 on success, -1 if the terminator is not found.
 */
int extract_to_terminator(statsite_conn_info *conn, char terminator, char **buf, int *buf_len, int *should_free) {
    // First we need to find the terminator...
    char *term_addr = NULL;
    if (conn->input.write_cursor < conn->input.read_cursor) {
        /*
         * We need to scan from the read cursor to the end of
         * the buffer, and then from the start of the buffer to
         * the write cursor.
        */
        term_addr = memchr(conn->input.buffer+conn->input.read_cursor,
                           terminator,
                           conn->input.buf_size - conn->input.read_cursor);

        // If we've found the terminator, we can just move up
        // the read cursor
        if (term_addr) {
            *buf = conn->input.buffer + conn->input.read_cursor;
            *buf_len = term_addr - *buf + 1;    // Difference between the terminator and location
            *term_addr = '\0';              // Add a null terminator
            *should_free = 0;               // No need to free, in the buffer
            conn->input.read_cursor = term_addr - conn->input.buffer + 1; // Push the read cursor forward
            return 0;
        }

        // Wrap around
        term_addr = memchr(conn->input.buffer,
                           terminator,
                           conn->input.write_cursor);

        // If we've found the terminator, we need to allocate
        // a contiguous buffer large enough to store everything
        // and provide a linear buffer
        if (term_addr) {
            int start_size = term_addr - conn->input.buffer + 1;
            int end_size = conn->input.buf_size - conn->input.read_cursor;
            *buf_len = start_size + end_size;
            *buf = malloc(*buf_len);

            // Copy from the read cursor to the end
            memcpy(*buf, conn->input.buffer+conn->input.read_cursor, end_size);

            // Copy from the start to the terminator
            *term_addr = '\0';              // Add a null terminator
            memcpy(*buf+end_size, conn->input.buffer, start_size);

            *should_free = 1;               // Must free, not in the buffer
            conn->input.read_cursor = start_size; // Push the read cursor forward
        }

    } else {
        /*
         * We need to scan from the read cursor to write buffer.
         */
        term_addr = memchr(conn->input.buffer+conn->input.read_cursor,
                           terminator,
                           conn->input.write_cursor - conn->input.read_cursor);

        // If we've found the terminator, we can just move up
        // the read cursor
        if (term_addr) {
            *buf = conn->input.buffer + conn->input.read_cursor;
            *buf_len = term_addr - *buf + 1; // Difference between the terminator and location
            *term_addr = '\0';               // Add a null terminator
            *should_free = 0;                // No need to free, in the buffer
            conn->input.read_cursor = term_addr - conn->input.buffer + 1; // Push the read cursor forward
        }
    }

    // Minor optimization, if our read-cursor has caught up
    // with the write cursor, reset them to the beginning
    // to avoid wrapping in the future
    if (conn->input.read_cursor == conn->input.write_cursor) {
        conn->input.read_cursor = 0;
        conn->input.write_cursor = 0;
    }

    // Return success if we have a term address
    return ((term_addr) ? 0 : -1);
}


/**
 * Sets the client socket options.
 * @return 0 on success, 1 on error.
 */
static int set_client_sockopts(int client_fd) {
    // Setup the socket to be non-blocking
    int sock_flags = fcntl(client_fd, F_GETFL, 0);
    if (sock_flags < 0) {
        syslog(LOG_ERR, "Failed to get socket flags on connection! %s.", strerror(errno));
        close(client_fd);
        return 1;
    }
    if (fcntl(client_fd, F_SETFL, sock_flags | O_NONBLOCK)) {
        syslog(LOG_ERR, "Failed to set O_NONBLOCK on connection! %s.", strerror(errno));
        close(client_fd);
        return 1;
    }

    /**
     * Set TCP_NODELAY. This will allow us to send small response packets more
     * quickly, since our responses are rarely large enough to consume a packet.
     */
    int flag = 1;
    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int))) {
        syslog(LOG_WARNING, "Failed to set TCP_NODELAY on connection! %s.", strerror(errno));
    }

    // Set keep alive
    if(setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(int))) {
        syslog(LOG_WARNING, "Failed to set SO_KEEPALIVE on connection! %s.", strerror(errno));
    }

    return 0;
}


/**
 * Returns the conn_info* object associated with the FD
 * or allocates a new one as necessary.
 */
static conn_info* get_conn(statsite_networking *netconf) {
    // Allocate space
    conn_info *conn = malloc(sizeof(conn_info));

    // Setup variables
    conn->netconf = netconf;
    conn->ref_count = 1;
    conn->should_schedule = 1;
    conn->use_write_buf = 0;

    // Prepare the buffers
    circbuf_init(&conn->input);
    circbuf_init(&conn->output);

    // Store a reference to the conn object
    conn->client.data = conn;
    conn->write_client.data = conn;

    return conn;
}

/*
 * Methods for manipulating our circular buffers
 */

// Conditionally allocates if there is no buffer
static void circbuf_init(circular_buffer *buf) {
    buf->read_cursor = 0;
    buf->write_cursor = 0;
    buf->buf_size = INIT_CONN_BUF_SIZE * sizeof(char);
    buf->buffer = malloc(buf->buf_size);
}

// Clears the circular buffer, reseting it.
static void circbuf_clear(circular_buffer *buf) {
    buf->read_cursor = 0;
    buf->write_cursor = 0;
}

// Frees a buffer
static void circbuf_free(circular_buffer *buf) {
    if (buf->buffer) free(buf->buffer);
    buf->buffer = NULL;
}

// Calculates the available buffer size
static uint64_t circbuf_avail_buf(circular_buffer *buf) {
    uint64_t avail_buf;
    if (buf->write_cursor < buf->read_cursor) {
        avail_buf = buf->read_cursor - buf->write_cursor - 1;
    } else {
        avail_buf = buf->buf_size - buf->write_cursor + buf->read_cursor - 1;
    }
    return avail_buf;
}

// Grows the circular buffer to make room for more data
static void circbuf_grow_buf(circular_buffer *buf) {
    int new_size = buf->buf_size * CONN_BUF_MULTIPLIER * sizeof(char);
    char *new_buf = malloc(new_size);
    int bytes_written = 0;

    // Check if the write has wrapped around
    if (buf->write_cursor < buf->read_cursor) {
        // Copy from the read cursor to the end of the buffer
        bytes_written = buf->buf_size - buf->read_cursor;
        memcpy(new_buf,
               buf->buffer+buf->read_cursor,
               bytes_written);

        // Copy from the start to the write cursor
        memcpy(new_buf+bytes_written,
               buf->buffer,
               buf->write_cursor);
        bytes_written += buf->write_cursor;

    // We haven't wrapped yet...
    } else {
        // Copy from the read cursor up to the write cursor
        bytes_written = buf->write_cursor - buf->read_cursor;
        memcpy(new_buf,
               buf->buffer + buf->read_cursor,
               bytes_written);
    }

    // Update the buffer locations and everything
    free(buf->buffer);
    buf->buffer = new_buf;
    buf->buf_size = new_size;
    buf->read_cursor = 0;
    buf->write_cursor = bytes_written;
}


// Initializes a pair of iovectors to be used for readv
static void circbuf_setup_readv_iovec(circular_buffer *buf, struct iovec *vectors, int *num_vectors) {
    // Check if we've wrapped around
    *num_vectors = 1;
    if (buf->write_cursor < buf->read_cursor) {
        vectors[0].iov_base = buf->buffer + buf->write_cursor;
        vectors[0].iov_len = buf->read_cursor - buf->write_cursor - 1;
    } else {
        vectors[0].iov_base = buf->buffer + buf->write_cursor;
        vectors[0].iov_len = buf->buf_size - buf->write_cursor - 1;
        if (buf->read_cursor > 0)  {
            vectors[0].iov_len += 1;
            vectors[1].iov_base = buf->buffer;
            vectors[1].iov_len = buf->read_cursor - 1;
            *num_vectors = 2;
        }
    }
}

// Initializes a pair of iovectors to be used for writev
static void circbuf_setup_writev_iovec(circular_buffer *buf, struct iovec *vectors, int *num_vectors) {
    // Check if we've wrapped around
    if (buf->write_cursor < buf->read_cursor) {
        *num_vectors = 2;
        vectors[0].iov_base = buf->buffer + buf->read_cursor;
        vectors[0].iov_len = buf->buf_size - buf->read_cursor;
        vectors[1].iov_base = buf->buffer;
        vectors[1].iov_len = buf->write_cursor;
    } else {
        *num_vectors = 1;
        vectors[0].iov_base = buf->buffer + buf->read_cursor;
        vectors[0].iov_len = buf->write_cursor - buf->read_cursor;
    }
}

// Advances the cursors
static void circbuf_advance_write(circular_buffer *buf, uint64_t bytes) {
    buf->write_cursor = (buf->write_cursor + bytes) % buf->buf_size;
}

static void circbuf_advance_read(circular_buffer *buf, uint64_t bytes) {
    buf->read_cursor = (buf->read_cursor + bytes) % buf->buf_size;

    // Optimization, reset the cursors if they catchup with each other
    if (buf->read_cursor == buf->write_cursor) {
        buf->read_cursor = 0;
        buf->write_cursor = 0;
    }
}

/**
 * Writes the data from a given input buffer
 * into the circular buffer.
 * @return 0 on success.
 */
static int circbuf_write(circular_buffer *buf, char *in, uint64_t bytes) {
    // Check for available space
    uint64_t avail = circbuf_avail_buf(buf);
    while (avail < bytes) {
        circbuf_grow_buf(buf);
        avail = circbuf_avail_buf(buf);
    }

    if (buf->write_cursor < buf->read_cursor) {
        memcpy(buf->buffer+buf->write_cursor, in, bytes);
        buf->write_cursor += bytes;

    } else {
        uint64_t end_size = buf->buf_size - buf->write_cursor;
        if (end_size >= bytes) {
            memcpy(buf->buffer+buf->write_cursor, in, bytes);
            buf->write_cursor += bytes;

        } else {
            // Copy the first end_size bytes
            memcpy(buf->buffer+buf->write_cursor, in, end_size);

            // Copy the remaining data
            memcpy(buf->buffer, in, (bytes - end_size));
            buf->write_cursor = (bytes - end_size);
        }
    }

    return 0;
}

