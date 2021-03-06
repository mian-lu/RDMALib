
//
// Created by Haoyu Huang on 4/1/20.
// Copyright (c) 2020 University of Southern California. All rights reserved.
//


#include "rdma_ctrl.hpp"
#include "nova_common.h"
#include "nova_config.h"
#include "nova_rdma_rc_broker.h"
#include "nova_mem_manager.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <assert.h>
#include <csignal>
#include <gflags/gflags.h>

using namespace std;
using namespace rdmaio;
using namespace nova;

NovaConfig *NovaConfig::config;

DEFINE_string(servers, "node-0:11211,node-1:11211,node-2:11211",
              "A list of servers");
DEFINE_int64(server_id, -1, "Server id.");

DEFINE_uint64(mem_pool_size_gb, 0, "Memory pool size in GB.");

DEFINE_uint64(rdma_port, 0, "The port used by RDMA to setup QPs.");
DEFINE_uint64(rdma_max_msg_size, 0, "The maximum message size used by RDMA.");
DEFINE_uint64(rdma_max_num_sends, 0,
              "The maximum number of pending RDMA sends. This includes READ/WRITE/SEND. We also post the same number of RECV events for each QP. ");
DEFINE_uint64(rdma_doorbell_batch_size, 0, "The doorbell batch size.");
DEFINE_uint32(nrdma_workers, 0,
              "Number of rdma threads.");

class ExampleRDMAThread {
public:
    void Start();

    RdmaCtrl *ctrl_;
    std::vector<QPEndPoint> endpoints_;
    char *rdma_backing_mem_;
    char *circular_buffer_;
};

void ExampleRDMAThread::Start() {
// A thread i at server j connects to thread i of all other servers.
    NovaRDMARCBroker *broker = new NovaRDMARCBroker(circular_buffer_, 0,
                                                    endpoints_,
                                                    FLAGS_rdma_max_num_sends,
                                                    FLAGS_rdma_max_msg_size,
                                                    FLAGS_rdma_doorbell_batch_size,
                                                    FLAGS_server_id,
                                                    rdma_backing_mem_,
                                                    FLAGS_mem_pool_size_gb *
                                                    1024 *
                                                    1024 * 1024,
                                                    FLAGS_rdma_port,
                                                    new DummyNovaMsgCallback);
    broker->Init(ctrl_);

    if (FLAGS_server_id == 0) {
        int server_id = 1;
        char *sendbuf = broker->GetSendBuf(server_id);
        // Write a request into the buf.
        sendbuf[0] = 'a';
        uint64_t wr_id = broker->PostSend(sendbuf, 1, server_id, 1);
        RDMA_LOG(INFO) << fmt::format("send one byte 'a' wr:{} imm:1", wr_id);
        broker->FlushPendingSends(server_id);
        broker->PollSQ(server_id);
        broker->PollRQ(server_id);
    }

    while (true) {
        broker->PollRQ();
        broker->PollSQ();
    }
}


int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_server_id == -1) {
        exit(0);
    }
    std::vector<gflags::CommandLineFlagInfo> flags;
    gflags::GetAllFlags(&flags);
    for (const auto &flag : flags) {
        printf("%s=%s\n", flag.name.c_str(),
               flag.current_value.c_str());
    }


    char *rdma_backing_mem = (char *) malloc(
            FLAGS_mem_pool_size_gb * 1024 * 1024 * 1024);
    memset(rdma_backing_mem, 0, FLAGS_mem_pool_size_gb * 1024 * 1024 * 1024);
    std::vector<Host> hosts = convert_hosts(FLAGS_servers);

    NovaConfig::config = new NovaConfig;
    NovaConfig::config->nrdma_threads = FLAGS_nrdma_workers;
    NovaConfig::config->my_server_id = FLAGS_server_id;
    NovaConfig::config->servers = hosts;
    NovaConfig::config->rdma_port = FLAGS_rdma_port;
    NovaConfig::config->rdma_doorbell_batch_size = FLAGS_rdma_doorbell_batch_size;
    NovaConfig::config->max_msg_size = FLAGS_rdma_max_msg_size;
    NovaConfig::config->rdma_max_num_sends = FLAGS_rdma_max_num_sends;

    RdmaCtrl *ctrl = new RdmaCtrl(FLAGS_server_id, FLAGS_rdma_port);
    std::vector<QPEndPoint> endpoints;
    for (int i = 0; i < hosts.size(); i++) {
        if (hosts[i].server_id == FLAGS_server_id) {
            continue;
        }
        QPEndPoint endpoint = {};
        endpoint.thread_id = 0;
        endpoint.server_id = hosts[i].server_id;
        endpoint.host = hosts[i];
        endpoints.push_back(endpoint);
    }

    // Each QP contains nrdma_buf_unit() memory for the circular buffer.
    // An RDMA broker uses nrdma_buf_unit() * number of servers memory for its circular buffers.
    // Each server contains nrdma_buf_unit() * number of servers * number of rdma threads for the circular buffers.

    // We register all memory to the RNIC.
    // RDMA verbs can only work on the memory registered in RNIC.
    // You may use nova mem manager to manage this memory.
    char *user_memory = rdma_backing_mem + nrdma_buf_total();
    uint32_t partitions = 1;
    uint32_t slab_mb = 1;
    NovaMemManager *mem_manager = new NovaMemManager(user_memory, partitions,
                                                     FLAGS_mem_pool_size_gb,
                                                     slab_mb);
    uint32_t scid = mem_manager->slabclassid(0, 40);
    char *buf = mem_manager->ItemAlloc(0, scid);
    // Do sth with the buf.
    mem_manager->FreeItem(0, buf, scid);


    ExampleRDMAThread *example = new ExampleRDMAThread;
    example->circular_buffer_ = rdma_backing_mem;
    example->ctrl_ = ctrl;
    example->endpoints_ = endpoints;
    example->rdma_backing_mem_ = rdma_backing_mem;
    std::thread t(&ExampleRDMAThread::Start, example);
    t.join();
    return 0;
}
