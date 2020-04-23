
//
// Created by Haoyu Huang on 4/4/19.
// Copyright (c) 2019 University of Southern California. All rights reserved.
//

#include <malloc.h>
#include <fmt/core.h>
#include "nova_rdma_rc_broker.h"

namespace nova {
    mutex open_device_mutex;
    bool is_device_opened = false;
    RNicHandler *device = nullptr;

    uint32_t NovaRDMARCBroker::to_qp_idx(uint32_t server_id) {
        return server_qp_idx_map[server_id];
    }

    // ML: char *buf is circular_buffer_ from main.cpp
    NovaRDMARCBroker::NovaRDMARCBroker(char *buf, int thread_id,
                                     const std::vector<nova::QPEndPoint> &end_points,
                                     uint32_t max_num_sends,
                                     uint32_t max_msg_size,
                                     uint32_t doorbell_batch_size,
                                     uint32_t my_server_id, char *mr_buf,
                                     uint64_t mr_size, uint64_t rdma_port,
                                     nova::NovaMsgCallback *callback) :
            rdma_buf_(buf),
            thread_id_(thread_id),
            end_points_(end_points),
            max_num_sends_(max_num_sends),
            max_msg_size_(max_msg_size),
            doorbell_batch_size_(doorbell_batch_size),
            my_server_id_(my_server_id),
            mr_buf_(mr_buf),
            mr_size_(mr_size),
            rdma_port_(rdma_port),
            callback_(callback) {
        RDMA_LOG(INFO)
            << fmt::format("rc[{}]: create rdma {} {} {} {} {} {}.",
                           thread_id_,
                           max_num_sends_,
                           max_msg_size_,
                           doorbell_batch_size_,
                           my_server_id_,
                           mr_size_,
                           rdma_port_);
        int max_num_wrs = max_num_sends;
        int num_servers = end_points_.size();

        wcs_ = (ibv_wc *) malloc(max_num_wrs * sizeof(ibv_wc));
        qp_ = (RCQP **) malloc(num_servers * sizeof(RCQP *));
        rdma_send_buf_ = (char **) malloc(num_servers * sizeof(char *)); // ML: Think of it as "aray of char*", therefore "one-char*-per-server"
        rdma_recv_buf_ = (char **) malloc(num_servers * sizeof(char *));
        send_sges_ = (struct ibv_sge **) malloc(
                num_servers * sizeof(struct ibv_sge *));
        send_wrs_ = (ibv_send_wr **) malloc(
                num_servers * sizeof(struct ibv_send_wr *));
        send_sge_index_ = (int *) malloc(num_servers * sizeof(int));

        npending_send_ = (int *) malloc(num_servers * sizeof(int));
        psend_index_ = (int *) malloc(num_servers * sizeof(int));

        uint64_t nsendbuf = max_num_sends * max_msg_size;
        uint64_t nrecvbuf = max_num_sends * max_msg_size;
        uint64_t nbuf = nsendbuf + nrecvbuf;

        char *rdma_buf_start = buf;
        for (int i = 0; i < num_servers; i++) {
            npending_send_[i] = 0;
            psend_index_[i] = 0;

            send_sge_index_[i] = 0;
            qp_[i] = NULL;

            rdma_recv_buf_[i] = rdma_buf_start + nbuf * i;
            memset(rdma_recv_buf_[i], 0, nrecvbuf);

            rdma_send_buf_[i] = rdma_recv_buf_[i] + nrecvbuf; // ML: point to right after the corresponding recv_buf (which starts at recv_buf_[i] and has length nrecvbuf)
            memset(rdma_send_buf_[i], 0, nsendbuf);

            send_sges_[i] = (ibv_sge *) malloc(
                    doorbell_batch_size * sizeof(struct ibv_sge));
            send_wrs_[i] = (ibv_send_wr *) malloc(
                    doorbell_batch_size * sizeof(struct ibv_send_wr));
            for (int j = 0; j < doorbell_batch_size; j++) {
                memset(&send_sges_[i][j], 0, sizeof(struct ibv_sge));
                memset(&send_wrs_[i][j], 0, sizeof(struct ibv_send_wr));
            }
            server_qp_idx_map[end_points[i].server_id] = i;
        }
        RDMA_LOG(INFO) << "rc[" << thread_id << "]: " << "created rdma";
    }

    void NovaRDMARCBroker::Init(RdmaCtrl *rdma_ctrl) {
        RDMA_LOG(INFO) << "RDMA client thread " << thread_id_
                       << " initializing";
        RdmaCtrl::DevIdx idx{.dev_id = 0, .port_id = 1}; // using the first RNIC's first port
        const char *cache_buf = mr_buf_;
        int num_servers = end_points_.size();
        uint64_t my_memory_id = my_server_id_;

        open_device_mutex.lock();
        if (!is_device_opened) {
            device = rdma_ctrl->open_device(idx);
            is_device_opened = true;
            RDMA_ASSERT(
                    rdma_ctrl->register_memory(my_memory_id,
                                               cache_buf,
                                               mr_size_,
                                               device));
        }
        open_device_mutex.unlock();

        RDMA_LOG(INFO) << "rdma-rc[" << thread_id_ << "]: register bytes "
                       << mr_size_
                       << " my memory id: "
                       << my_memory_id;
        for (int peer_id = 0; peer_id < num_servers; peer_id++) {
            QPEndPoint peer_store = end_points_[peer_id];
            QPIdx my_rc_key = create_rc_idx(my_server_id_, thread_id_,
                                            peer_store.server_id);
            QPIdx peer_rc_key = create_rc_idx(peer_store.server_id,
                                              peer_store.thread_id,
                                              my_server_id_);
            uint64_t peer_memory_id = static_cast<uint64_t >(peer_store.server_id);
            RDMA_LOG(INFO) << "rdma-rc[" << thread_id_
                           << "]: my rc key " << my_rc_key.node_id << ":"
                           << my_rc_key.worker_id << ":" << my_rc_key.index;

            RDMA_LOG(INFO) << "rdma-rc[" << thread_id_
                           << "]: connecting to peer rc key "
                           << peer_store.host.ip << ":" << peer_rc_key.node_id
                           << ":" << peer_rc_key.worker_id << ":"
                           << peer_rc_key.index;
            MemoryAttr local_mr = rdma_ctrl->get_local_mr(
                    my_memory_id);
            ibv_cq *cq = rdma_ctrl->create_cq(
                    device, max_num_sends_);
            ibv_cq *recv_cq = rdma_ctrl->create_cq(
                    device, max_num_sends_);
            qp_[peer_id] = rdma_ctrl->create_rc_qp(my_rc_key,
                                                   device,
                                                   &local_mr,
                                                   cq, recv_cq);
            // get remote server's memory information
            MemoryAttr remote_mr;
            while (QP::get_remote_mr(peer_store.host.ip,
                                     rdma_port_,
                                     peer_memory_id, &remote_mr) != SUCC) {
                usleep(CONN_SLEEP);
            }
            qp_[peer_id]->bind_remote_mr(remote_mr);
            RDMA_LOG(INFO) << "rdma-rc[" << thread_id_
                           << "]: connect to server "
                           << peer_store.host.ip << ":" << peer_store.host.port
                           << ":" << peer_store.thread_id;
            // bind to the previous allocated mr
            while (qp_[peer_id]->connect(peer_store.host.ip,
                                         rdma_port_,
                                         peer_rc_key) != SUCC) {
                usleep(CONN_SLEEP);
            }
            RDMA_LOG(INFO)
                << fmt::format(
                        "rdma-rc[{}]: connected to server {}:{}:{}. Posting {} recvs.",
                        thread_id_, peer_store.host.ip,
                        peer_store.host.port, peer_store.thread_id,
                        max_num_sends_);

            for (int i = 0; i < max_num_sends_; i++) {
                PostRecv(peer_store.server_id, i);
            }
        }
        RDMA_LOG(INFO)
            << fmt::format("RDMA client thread {} initialized", thread_id_);
    }

    uint64_t
    NovaRDMARCBroker::PostRDMASEND(const char *localbuf, ibv_wr_opcode opcode,
                                  uint32_t size,
                                  int server_id,
                                  uint64_t local_offset,
                                  uint64_t remote_addr, bool is_offset,
                                  uint32_t imm_data) {
        uint32_t qp_idx = to_qp_idx(server_id);
        uint64_t wr_id = psend_index_[qp_idx];
        const char *sendbuf = rdma_send_buf_[qp_idx] + wr_id * max_msg_size_;
        if (localbuf != nullptr) {
            sendbuf = localbuf;
        }
        int ssge_idx = send_sge_index_[qp_idx];
        ibv_sge *ssge = send_sges_[qp_idx];
        ibv_send_wr *swr = send_wrs_[qp_idx];
        ssge[ssge_idx].addr = (uintptr_t) sendbuf + local_offset;
        ssge[ssge_idx].length = size;
        ssge[ssge_idx].lkey = qp_[qp_idx]->local_mr_.key;
        swr[ssge_idx].wr_id = wr_id;
        swr[ssge_idx].sg_list = &ssge[ssge_idx];
        swr[ssge_idx].num_sge = 1;
        swr[ssge_idx].opcode = opcode;
        swr[ssge_idx].imm_data = imm_data;
        swr[ssge_idx].send_flags = IBV_SEND_SIGNALED;
        if (is_offset) {
            swr[ssge_idx].wr.rdma.remote_addr =
                    qp_[qp_idx]->remote_mr_.buf + remote_addr;
        } else {
            swr[ssge_idx].wr.rdma.remote_addr = remote_addr;
        }
        swr[ssge_idx].wr.rdma.rkey = qp_[qp_idx]->remote_mr_.key;
        if (ssge_idx + 1 < doorbell_batch_size_) {
            swr[ssge_idx].next = &swr[ssge_idx + 1];
        } else {
            swr[ssge_idx].next = NULL;
        }
        psend_index_[qp_idx]++;
        npending_send_[qp_idx]++;
        send_sge_index_[qp_idx]++;
        RDMA_LOG(DEBUG) << fmt::format(
                    "rdma-rc[{}]: SQ: rdma {} request to server {} wr:{} imm:{} roffset:{} isoff:{} size:{} p:{}:{}",
                    thread_id_, ibv_wr_opcode_str(opcode), server_id, wr_id,
                    imm_data,
                    remote_addr, is_offset, size, psend_index_[qp_idx],
                    npending_send_[qp_idx]);
        if (send_sge_index_[qp_idx] == doorbell_batch_size_) {
            // post send a batch of requests.
            send_sge_index_[qp_idx] = 0;
            ibv_send_wr *bad_sr;
            int ret = ibv_post_send(qp_[qp_idx]->qp_, &swr[0], &bad_sr);
            RDMA_ASSERT(ret == 0) << ret;
            RDMA_LOG(DEBUG) << "rdma-rc[" << thread_id_ << "]: "
                            << "SQ: posting "
                            << doorbell_batch_size_
                            << " requests";
        }

        while (npending_send_[qp_idx] == max_num_sends_) {
            // poll sq as it is full.
            PollSQ(server_id);
        }

        if (psend_index_[qp_idx] == max_num_sends_) {
            psend_index_[qp_idx] = 0;
        }
        return wr_id;
    }

    // ML: PostRead() is "initiating an action" to "read via RDMA" from a remote
    // node. This is the second step in the server-side redirection workflow.
    // Here, simply invoking PostRDMASEND() generic function, and indicate that
    // we're issuing a "IBV_WR_RDMA_READ" action.
    uint64_t
    NovaRDMARCBroker::PostRead(char *localbuf, uint32_t size, int server_id,
                              uint64_t local_offset,
                              uint64_t remote_addr, bool is_offset) {
        return PostRDMASEND(localbuf, IBV_WR_RDMA_READ, size, server_id,
                            local_offset,
                            remote_addr, is_offset, 0);
    }

    uint64_t
    NovaRDMARCBroker::PostSend(const char *localbuf, uint32_t size,
                              int server_id,
                              uint32_t imm_data) {
        ibv_wr_opcode wr = IBV_WR_SEND;
        if (imm_data != 0) {
            wr = IBV_WR_SEND_WITH_IMM;
        }
        RDMA_ASSERT(size < max_msg_size_);
        return PostRDMASEND(localbuf, wr, size, server_id, 0, 0, false,
                            imm_data);
    }

    void NovaRDMARCBroker::FlushPendingSends(int remote_server_id) {
        if (remote_server_id == my_server_id_) {
            return;
        }
        uint32_t qp_idx = to_qp_idx(remote_server_id);
        if (send_sge_index_[qp_idx] == 0) {
            return;
        }
        RDMA_LOG(DEBUG) << "rdma-rc[" << thread_id_ << "]: "
                        << "flush pending sends "
                        << send_sge_index_[qp_idx];
        send_wrs_[qp_idx][send_sge_index_[qp_idx] - 1].next = NULL;
        send_sge_index_[qp_idx] = 0;
        ibv_send_wr *bad_sr;
        int ret = ibv_post_send(qp_[qp_idx]->qp_, &send_wrs_[qp_idx][0],
                                &bad_sr);
        RDMA_ASSERT(ret == 0) << ret;
    }


    void NovaRDMARCBroker::FlushPendingSends() {
        for (int peer_id = 0; peer_id < end_points_.size(); peer_id++) {
            QPEndPoint peer_store = end_points_[peer_id];
            FlushPendingSends(peer_store.server_id);
        }
    }

    uint64_t
    NovaRDMARCBroker::PostWrite(const char *localbuf, uint32_t size,
                               int server_id,
                               uint64_t remote_offset, bool is_remote_offset,
                               uint32_t imm_data) {
        ibv_wr_opcode wr = IBV_WR_RDMA_WRITE;
        if (imm_data != 0) {
            wr = IBV_WR_RDMA_WRITE_WITH_IMM;
        }
        return PostRDMASEND(localbuf, wr, size, server_id, 0,
                            remote_offset, is_remote_offset, imm_data);
    }

    uint32_t NovaRDMARCBroker::PollSQ(int server_id) {
        if (server_id == my_server_id_) {
            return 0;
        }
        uint32_t qp_idx = to_qp_idx(server_id);
        int npending = npending_send_[qp_idx];
        if (npending == 0) {
            return 0;
        }

        // FIFO.
        int n = ibv_poll_cq(qp_[qp_idx]->cq_, max_num_sends_, wcs_);
        for (int i = 0; i < n; i++) {
            RDMA_ASSERT(wcs_[i].status == IBV_WC_SUCCESS)
                << "rdma-rc[" << thread_id_ << "]: " << "SQ error wc status "
                << wcs_[i].status << " str:"
                << ibv_wc_status_str(wcs_[i].status) << " serverid "
                << server_id;

            RDMA_LOG(DEBUG) << fmt::format(
                        "rdma-rc[{}]: SQ: poll complete from server {} wr:{} op:{}",
                        thread_id_, server_id, wcs_[i].wr_id,
                        ibv_wc_opcode_str(wcs_[i].opcode));
            char *buf = rdma_send_buf_[qp_idx] +
                        wcs_[i].wr_id * max_msg_size_;
            callback_->ProcessRDMAWC(wcs_[i].opcode, wcs_[i].wr_id, server_id,
                                     buf, wcs_[i].imm_data);
            // Send is complete.
            buf[0] = '~';
            npending_send_[qp_idx] -= 1;
        }
        return n;
    }

    uint32_t NovaRDMARCBroker::PollSQ() {
        uint32_t size = 0;
        for (int peer_id = 0; peer_id < end_points_.size(); peer_id++) {
            QPEndPoint peer_store = end_points_[peer_id];
            size += PollSQ(peer_store.server_id);
        }
        return size;
    }

    // ML: could this be the "first step in server-side redirection", i.e. have
    // P2_b receive P2_a's memory (addr, len), that I'm looking for?
    // TODO
    void NovaRDMARCBroker::PostRecv(int server_id, int recv_buf_index) {
        uint32_t qp_idx = to_qp_idx(server_id);
        char *local_buf =
                rdma_recv_buf_[qp_idx] + max_msg_size_ * recv_buf_index;
        local_buf[0] = '~';
        auto ret = qp_[qp_idx]->post_recv(local_buf, max_msg_size_,
                                          recv_buf_index);
        RDMA_ASSERT(ret == SUCC) << ret;
    }

    void NovaRDMARCBroker::FlushPendingRecvs() {}

    uint32_t NovaRDMARCBroker::PollRQ(int server_id) {
        uint32_t qp_idx = to_qp_idx(server_id);
        int n = ibv_poll_cq(qp_[qp_idx]->recv_cq_, max_num_sends_, wcs_);
        for (int i = 0; i < n; i++) {
            uint64_t wr_id = wcs_[i].wr_id;
            RDMA_ASSERT(wr_id < max_num_sends_);
            RDMA_ASSERT(wcs_[i].status == IBV_WC_SUCCESS)
                << "rdma-rc[" << thread_id_ << "]: " << "RQ error wc status "
                << ibv_wc_status_str(wcs_[i].status);

            RDMA_LOG(DEBUG)
                << fmt::format(
                        "rdma-rc[{}]: RQ: received from server {} wr:{} imm:{}",
                        thread_id_, server_id, wr_id, wcs_[i].imm_data);
            char *buf = rdma_recv_buf_[qp_idx] + max_msg_size_ * wr_id;
            callback_->ProcessRDMAWC(wcs_[i].opcode, wcs_[i].wr_id, server_id,
                                     buf, wcs_[i].imm_data);
            // Post another receive event.
            PostRecv(server_id, wr_id);
        }

        // Flush all pending send requests.
        FlushPendingSends(server_id);
        return n;
    }

    uint32_t NovaRDMARCBroker::PollRQ() {
        uint32_t size = 0;
        for (int peer_id = 0; peer_id < end_points_.size(); peer_id++) {
            QPEndPoint peer_store = end_points_[peer_id];
            size += PollRQ(peer_store.server_id);
        }
        return size;
    }

    // ML: so the point is to just dis-allow getting sendbuf without a
    // (destination) server_id? Or is it useful sometimes calling this?
    char *NovaRDMARCBroker::GetSendBuf() {
        return NULL;
    }

    char *NovaRDMARCBroker::GetSendBuf(int server_id) {
        uint32_t qp_idx = to_qp_idx(server_id);
        // ML: what is the size of this sendbuf that gets returned??
        // I feel like the size is simply max_msg_size_.
        return rdma_send_buf_[qp_idx] +
               psend_index_[qp_idx] * max_msg_size_;
    }
}