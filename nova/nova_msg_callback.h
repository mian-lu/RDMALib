
//
// Created by Haoyu Huang on 4/1/19.
// Copyright (c) 2019 University of Southern California. All rights reserved.
//

#ifndef RLIB_NOVA_MSG_CALLBACK_H
#define RLIB_NOVA_MSG_CALLBACK_H

#include "rdma_ctrl.hpp"
namespace nova {

    class NovaMsgCallback {
    public:
        virtual bool
        ProcessRDMAWC(ibv_wc_opcode type, uint64_t wr_id, int remote_server_id,
                      char *buf, uint32_t imm_data) = 0;
    };

    class DummyNovaMsgCallback : public NovaMsgCallback {
    public:
        bool
        ProcessRDMAWC(ibv_wc_opcode type, uint64_t wr_id, int remote_server_id,
                      char *buf, uint32_t imm_data) override {
            return true;
        }
    };
}
#endif //RLIB_NOVA_MSG_CALLBACK_H
