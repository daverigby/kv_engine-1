/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include "mcbp_test.h"

#include <daemon/connection.h>
#include <event2/event.h>
#include <memcached/protocol_binary.h>

namespace mcbp {
namespace test {


// Test the validators for GAT, GATQ, GATK, GATKQ, GAT_META and GATQ_META
class GATValidatorTest
    : public ::testing::WithParamInterface<
              std::tuple<cb::mcbp::ClientOpcode, bool /*collections on/off*/>>,
      public ValidatorTest {
public:
    void SetUp() override {
        ValidatorTest::SetUp();
        request.message.header.request.extlen = 4;
        request.message.header.request.keylen = htons(10);
        request.message.header.request.bodylen = htonl(14);
    }

    GATValidatorTest()
        : ValidatorTest(std::get<1>(GetParam())),
          bodylen(request.message.header.request.bodylen) {
        // empty
    }

protected:
    cb::mcbp::Status validateExtendedExtlen(uint8_t version) {
        bodylen = htonl(ntohl(bodylen) + 1);
        request.message.header.request.extlen = 1;
        blob[sizeof(protocol_binary_request_gat)] = version;
        return validate();
    }

    cb::mcbp::Status validate() {
        auto opcode = cb::mcbp::ClientOpcode(std::get<0>(GetParam()));
        return ValidatorTest::validate(opcode, static_cast<void*>(&request));
    }

    uint32_t& bodylen;
};

TEST_P(GATValidatorTest, CorrectMessage) {
    EXPECT_EQ(cb::mcbp::Status::Success, validate());
}

TEST_P(GATValidatorTest, InvalidMagic) {
    request.message.header.request.magic = 0;
    EXPECT_EQ(cb::mcbp::Status::Einval, validate());
}

TEST_P(GATValidatorTest, InvalidExtlen) {
    bodylen = htonl(15);
    request.message.header.request.extlen = 5;
    EXPECT_EQ(cb::mcbp::Status::Einval, validate());
}

TEST_P(GATValidatorTest, NoKey) {
    request.message.header.request.keylen = 0;
    bodylen = ntohl(4);
    EXPECT_EQ(cb::mcbp::Status::Einval, validate());
}

TEST_P(GATValidatorTest, InvalidDatatype) {
    request.message.header.request.datatype = PROTOCOL_BINARY_DATATYPE_JSON;
    EXPECT_EQ(cb::mcbp::Status::Einval, validate());
}

TEST_P(GATValidatorTest, InvalidCas) {
    request.message.header.request.cas = 1;
    EXPECT_EQ(cb::mcbp::Status::Einval, validate());
}

INSTANTIATE_TEST_CASE_P(
        GATOpcodes,
        GATValidatorTest,
        ::testing::Combine(::testing::Values(cb::mcbp::ClientOpcode::Gat,
                                             cb::mcbp::ClientOpcode::Gatq,
                                             cb::mcbp::ClientOpcode::Touch),
                           ::testing::Bool()), );
} // namespace test
} // namespace mcbp
