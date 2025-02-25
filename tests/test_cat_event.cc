/*
  +--------------------------------------------------------------------------+
  | libcat                                                                   |
  +--------------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License");          |
  | you may not use this file except in compliance with the License.         |
  | You may obtain a copy of the License at                                  |
  | http://www.apache.org/licenses/LICENSE-2.0                               |
  | Unless required by applicable law or agreed to in writing, software      |
  | distributed under the License is distributed on an "AS IS" BASIS,        |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. |
  | See the License for the specific language governing permissions and      |
  | limitations under the License. See accompanying LICENSE file.            |
  +--------------------------------------------------------------------------+
  | Author: Twosee <twosee@php.net>                                          |
  |         codinghuang <2812240764@qq.com>                                  |
  +--------------------------------------------------------------------------+
 */

#include "test.h"

#ifndef CAT_OS_WIN
TEST(cat_event, fork)
{
    // it should has not effect for anything
    cat_event_fork();
}
#endif

TEST(cat_event, defer)
{
    bool done = false;
    ASSERT_TRUE(cat_event_defer([](cat_data_t *data) {
        *((bool *) data) = true;
    }, &done));
    ASSERT_TRUE(cat_time_delay(0));
    ASSERT_TRUE(done);
}

TEST(cat_event, defer_in_defer)
{
    uint64_t round = cat_event_loop->round;
    bool done1 = false;
    bool done2 = false;
    bool done3 = false;
    ASSERT_TRUE(defer([&] {
        ASSERT_TRUE(defer([&] {
            ASSERT_TRUE(defer([&] {
                ASSERT_EQ(cat_event_loop->round, round + 3);
                done3 = true;
            }));
            ASSERT_EQ(cat_event_loop->round, round + 2);
            done2 = true;
        }));
        ASSERT_EQ(cat_event_loop->round, round + 1);
        done1 = true;
    }));
    ASSERT_TRUE(cat_time_delay(0));
    ASSERT_TRUE(done1);
    ASSERT_TRUE(cat_time_delay(0));
    ASSERT_TRUE(done2);
    ASSERT_TRUE(cat_time_delay(0));
    ASSERT_TRUE(done3);
}

TEST(cat_event, defer_not_blocking)
{
    cat_event_wait();
    ASSERT_TRUE(defer([]{}));
    cat_event_wait(); // not blocking
}

// defer tasks should have no affect on backend time
TEST(cat_event, defer_backend_time)
{
    int timeout;

    cat_event_wait();
    timeout = uv_backend_timeout(cat_event_loop);
    ASSERT_TRUE(defer([]{}));
    ASSERT_EQ(uv_backend_timeout(cat_event_loop), timeout);

    cat_event_wait();
    timeout = uv_backend_timeout(cat_event_loop);
    co([] {
        ASSERT_TRUE(cat_time_delay(1));
    });
    ASSERT_GT(uv_backend_timeout(cat_event_loop), timeout);
    timeout = uv_backend_timeout(cat_event_loop);
    ASSERT_TRUE(defer([]{}));
    ASSERT_EQ(uv_backend_timeout(cat_event_loop), timeout);
}

TEST(cat_event, wait)
{
    bool done = false;
    co([&] {
        ASSERT_TRUE(cat_time_delay(1));
        done = true;
    });
    cat_event_wait();
    ASSERT_TRUE(done);
}

TEST(cat_event, runtime_shutdown_function)
{
    ASSERT_NE(cat_event_register_runtime_shutdown_task([] (cat_data_t *data) {
        ASSERT_EQ((int) (intptr_t) data, CAT_MAGIC_NUMBER);
    }, (cat_data_t *) (intptr_t) CAT_MAGIC_NUMBER), nullptr);

    cat_event_task_t *task;
    ASSERT_NE((task = cat_event_register_runtime_shutdown_task([] (cat_data_t *data) {
        ASSERT_TRUE(0 && "never here");
    }, (cat_data_t *) (intptr_t) CAT_MAGIC_NUMBER)), nullptr);
    cat_event_unregister_runtime_shutdown_task(task);
}

#ifdef CAT_OS_UNIX_LIKE

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

TEST(cat_event, real_fork)
{
    cat_socket_t server;
    cat_socket_t dummy, dummy_peer;
    int port;
    char buffer[TEST_BUFFER_SIZE_STD];
    ssize_t nread;

    ASSERT_EQ(cat_socket_create(&server, CAT_SOCKET_TYPE_TCP), &server);
    DEFER(cat_socket_close(&server));
    ASSERT_TRUE(cat_socket_bind(&server, CAT_STRL(TEST_LISTEN_HOST), 0));
    ASSERT_TRUE(cat_socket_listen(&server, TEST_SERVER_BACKLOG));
    port = cat_socket_get_sock_port(&server);
    ASSERT_GT(port, 0);

    // dummy should be closed in sub-process
    ASSERT_EQ(cat_socket_create(&dummy, CAT_SOCKET_TYPE_TCP), &dummy);
    DEFER(cat_socket_close(&dummy));
    ASSERT_TRUE(cat_socket_connect(&dummy, CAT_STRL(TEST_LISTEN_HOST), port));
    cat_socket_init(&dummy_peer);
    ASSERT_EQ(cat_socket_accept_ex(&server, &dummy_peer, TEST_IO_TIMEOUT), &dummy_peer);
    ASSERT_TRUE(cat_socket_send(&dummy_peer, CAT_STRS("PING")));
    DEFER(cat_socket_close(&dummy_peer));

    cat_pid_t pid = fork();
    SKIP_IF_(pid < 0, "Fork failed");
    if (pid > 0) {
        cat_msec_t s = cat_time_msec();
        cat_socket_t client;
        cat_socket_init(&client);
        ASSERT_EQ(cat_socket_accept_ex(&server, &client, TEST_IO_TIMEOUT), &client);
        DEFER(cat_socket_close(&client));
        ASSERT_GT(nread = cat_socket_recv(&client, CAT_STRS(buffer)), 0);
        ASSERT_EQ(std::string(buffer, nread - 1), "Forked\n");
        int wstatus;
        cat_pid_t dead_pid;
        dead_pid = waitpid(pid, &wstatus, 0);
        ASSERT_EQ(dead_pid, pid);
        ASSERT_EQ(WIFEXITED(wstatus), 1);
        ASSERT_EQ(WEXITSTATUS(wstatus), 0);
        s = cat_time_msec() - s;
        ASSERT_GE(s, 5);
    } else if (pid == 0) {
        cat_event_fork();
        cat_socket_close_all();
        if (cat_socket_check_liveness(&dummy)) {
            fprintf(stderr, "dummy socket should be closed\n");
            _exit(1);
        }
        if (cat_time_delay(10) != CAT_RET_OK) {
            fprintf(stderr, "time delay failed\n");
            _exit(1);
        }
        cat_socket_t client;
        if (cat_socket_create(&client, CAT_SOCKET_TYPE_TCP) != &client) {
            fprintf(stderr, "socket create failed\n");
            _exit(1);
        }
        DEFER(cat_socket_close(&client));
        if (!cat_socket_connect(&client, CAT_STRL(TEST_LISTEN_HOST), port)) {
            fprintf(stderr, "socket connect failed\n");
            _exit(1);
        }
        if (!cat_socket_send(&client, CAT_STRS("Forked\n"))) {
            fprintf(stderr, "socket send failed\n");
            _exit(1);
        }
        _exit(0);
    } else {
        CAT_NEVER_HERE("Should be skipped");
    }
}
#endif

TEST(cat_event, busy)
{
    co([] {
        ASSERT_TRUE(cat_time_delay(0));
    });
    testing::internal::CaptureStderr();
    ASSERT_FALSE(cat_event_module_shutdown());
    ASSERT_EQ(cat_get_last_error_code(), CAT_EBUSY);
    std::string output = testing::internal::GetCapturedStderr();
    ASSERT_NE(output.find("Event loop close failed, reason: Resource busy or locked"), std::string::npos);
}

TEST(cat_event, dead_lock)
{
    co([] {
        ASSERT_TRUE(cat_time_delay(0));
    });
    cat_event_dead_lock();
}
