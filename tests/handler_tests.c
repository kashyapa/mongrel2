
#include "minunit.h"
#include <handler.h>
#include <string.h>
#include <task/task.h>

FILE *LOG_FILE = NULL;


char *test_Handler_recv_create()
{
    void *socket = Handler_recv_create("tcp://127.0.0.1:4321", "ZED");
    mu_assert(socket != NULL, "Failed to make recv socket.");
    zmq_close(socket);
    return NULL;
}

char *test_Handler_send_create()
{

    void *socket = Handler_send_create("tcp://127.0.0.1:12345", "ZED");
    mu_assert(socket != NULL, "Failed to make the send socket.");

    zmq_close(socket);

    return NULL;
}

char *test_Handler_deliver()
{
    void *socket = Handler_send_create("tcp://127.0.0.1:12346", "ZED");
#ifdef ZMQ_LINGER
    int opt = 0;
    zmq_setsockopt(socket, ZMQ_LINGER, &opt, sizeof(opt));
#endif

    mu_assert(socket != NULL, "Failed to make the send socket.");

    bstring message = bfromcstr("{\"type\":\"join\"}");
    int rc = Handler_deliver(socket, bdata(message), blength(message));
    free(message);  // handler owns the message->data so we just free

    mu_assert(rc == 0, "Failed to deliver the message.");

    zmq_close(socket);
    return NULL;
}


char *test_Handler_create_destroy()
{
    Handler *handler = Handler_create("tcp://127.0.0.1:12348", "ZED", "tcp://127.0.0.1:4321", "ZED");
    mu_assert(handler != NULL, "Failed to make the handler.");

    Handler_destroy(handler);

    return NULL;
}

char * all_tests() {
    mu_suite_start();
    mqinit(2);

    mu_run_test(test_Handler_send_create);
    mu_run_test(test_Handler_recv_create);
    // disabled for now mu_run_test(test_Handler_deliver);
    mu_run_test(test_Handler_create_destroy);

    zmq_term(ZMQ_CTX);
    return NULL;
}

RUN_TESTS(all_tests);

