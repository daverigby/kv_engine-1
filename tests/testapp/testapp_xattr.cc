/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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
#include "testapp_xattr.h"

#include <platform/crc32c.h>
#include <platform/string_hex.h>

#include <array>

// @todo add the other transport protocols
// Note: We always need XattrSupport::Yes for these tests
INSTANTIATE_TEST_CASE_P(
        TransportProtocols,
        XattrTest,
        ::testing::Combine(::testing::Values(TransportProtocols::McbpPlain),
                           ::testing::Values(XattrSupport::Yes),
                           ::testing::Values(ClientJSONSupport::Yes,
                                             ClientJSONSupport::No),
                           ::testing::Values(ClientSnappySupport::Yes)),
        PrintToStringCombinedName());

// Instantiation for tests which want XATTR support disabled.
INSTANTIATE_TEST_CASE_P(
        TransportProtocols,
        XattrDisabledTest,
        ::testing::Combine(::testing::Values(TransportProtocols::McbpPlain),
                           ::testing::Values(XattrSupport::No),
                           ::testing::Values(ClientJSONSupport::Yes,
                                             ClientJSONSupport::No),
                           ::testing::Values(ClientSnappySupport::No)),
        PrintToStringCombinedName());

TEST_P(XattrTest, GetXattrAndBody) {
    // Test to check that we can get both an xattr and the main body in
    // subdoc multi-lookup
    setBodyAndXattr(value, {{sysXattr, xattrVal}});

    // Sanity checks and setup done lets try the multi-lookup

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(xattrVal, multiResp.getResults()[0].value);
    EXPECT_EQ(value, multiResp.getResults()[1].value);
}

TEST_P(XattrTest, SetXattrAndBodyNewDoc) {
    // Ensure we are working on a new doc
    getConnection().remove(name, Vbid(0));
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Mkdoc);

    testBodyAndXattrCmd(cmd);
}

TEST_P(XattrTest, SetXattrAndBodyNewDocWithExpiry) {
    // For MB-24542
    // Ensure we are working on a new doc
    getConnection().remove(name, Vbid(0));
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.setExpiry(3);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Mkdoc);

    testBodyAndXattrCmd(cmd);

    // Jump forward in time to expire item
    adjust_memcached_clock(4, TimeType::Uptime);

    auto& conn = getConnection();
    BinprotSubdocMultiLookupCommand getCmd;
    getCmd.setKey(name);
    getCmd.addLookup("", cb::mcbp::ClientOpcode::Get);
    conn.sendCommand(getCmd);

    BinprotSubdocMultiLookupResponse getResp;
    conn.recvResponse(getResp);
    EXPECT_EQ(cb::mcbp::Status::KeyEnoent, getResp.getStatus());

    // Restore time.
    adjust_memcached_clock(0, TimeType::Uptime);
}

TEST_P(XattrTest, SetXattrAndBodyExistingDoc) {
    // Ensure that a doc is already present
    setBodyAndXattr("{\"TestField\":56788}", {{sysXattr, "4543"}});
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);

    testBodyAndXattrCmd(cmd);
}

TEST_P(XattrTest, SetXattrAndBodyInvalidFlags) {
    // First test invalid path flags
    std::array<protocol_binary_subdoc_flag, 3> badFlags = {
            {SUBDOC_FLAG_MKDIR_P,
             SUBDOC_FLAG_XATTR_PATH,
             SUBDOC_FLAG_EXPAND_MACROS}};

    for (const auto& flag : badFlags) {
        BinprotSubdocMultiMutationCommand cmd;
        cmd.setKey(name);

        // Should not be able to set all XATTRs
        cmd.addMutation(cb::mcbp::ClientOpcode::Set, flag, "", value);

        auto& conn = getConnection();
        conn.sendCommand(cmd);

        BinprotSubdocMultiMutationResponse multiResp;
        conn.recvResponse(multiResp);
        EXPECT_EQ(cb::mcbp::Status::Einval, multiResp.getStatus());
    }

    // Now test the invalid doc flags
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);

    // Should not be able to set all XATTRs
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Einval, multiResp.getStatus());
}

TEST_P(XattrTest, SetBodyInMultiLookup) {
    // Check that we can't put a CMD_SET in a multi lookup
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    // Should not be able to put a set in a multi lookup
    cmd.addLookup("", cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE);
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocInvalidCombo, multiResp.getStatus());
}

TEST_P(XattrTest, GetBodyInMultiMutation) {
    // Check that we can't put a CMD_GET in a multi mutation
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);

    // Should not be able to put a get in a multi multi-mutation
    cmd.addMutation(cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE, "", value);
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocInvalidCombo, multiResp.getStatus());
}

TEST_P(XattrTest, AddBodyAndXattr) {
    // Check that we can use the Add doc flag to create a new document

    // Get rid of any existing doc
    getConnection().remove(name, Vbid(0));

    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Add);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
}

TEST_P(XattrTest, AddBodyAndXattrAlreadyExistDoc) {
    // Check that usage of the Add flag will return EEXISTS if a key already
    // exists

    // Make sure a doc exists
    setBodyAndXattr("{\"TestField\":56788}", {{sysXattr, "4543"}});

    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Add);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::KeyEexists, multiResp.getStatus());
}

TEST_P(XattrTest, AddBodyAndXattrInvalidDocFlags) {
    // Check that usage of the Add flag will return EINVAL if the mkdoc doc
    // flag is also passed. The preexisting document exists to check that
    // we fail with the right error. i.e. we shouldn't even be fetching
    // the document from the engine if these two flags are set.

    // Make sure a doc exists
    setBodyAndXattr("{\"TestField\":56788}", {{sysXattr, "4543"}});

    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Add);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::Mkdoc);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Einval, multiResp.getStatus());
}

TEST_P(XattrTest, TestSeqnoMacroExpansion) {
    // Test that we don't replace it when we don't send EXPAND_MACROS
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "_sync.seqno",
                       "\"${Mutation.seqno}\"",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc_get("_sync.seqno", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("\"${Mutation.seqno}\"", resp.getValue());

    // Verify that we expand the macro to something that isn't the macro
    // literal. i.e. If we send ${Mutation.SEQNO} we want to check that we
    // replaced that with something else (hopefully the correct value).
    // Unfortunately, unlike the cas, we do not get the seqno so we cannot
    // check it.
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "_sync.seqno",
                  "\"${Mutation.seqno}\"",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_EXPAND_MACROS);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("_sync.seqno", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_NE("\"${Mutation.seqno}\"", resp.getValue());
}

TEST_P(XattrTest, TestMacroExpansionAndIsolation) {
    // This test verifies that you can have the same path in xattr's
    // and in the document without one affecting the other.
    // In addition to that we're testing that macro expansion works
    // as expected.

    // Lets store the macro and verify that it isn't expanded without the
    // expand macro flag
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "_sync.cas",
                       "\"${Mutation.CAS}\"",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("_sync.cas", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ("\"${Mutation.CAS}\"", resp.getValue());

    // Let's update the body version..
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "_sync.cas",
                  "\"If you don't know me by now\"",
                  SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // The xattr version should have been unchanged...
    resp = subdoc_get("_sync.cas", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ("\"${Mutation.CAS}\"", resp.getValue());

    // And the body version should be what we set it to
    resp = subdoc_get("_sync.cas");
    EXPECT_EQ("\"If you don't know me by now\"", resp.getValue());

    // Then change it to macro expansion
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "_sync.cas",
                  "\"${Mutation.CAS}\"",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_EXPAND_MACROS);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    /// Fetch the field and verify that it expanded the cas!
    std::string cas_string;
    resp = subdoc_get("_sync.cas", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    auto first_cas = resp.getCas();
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << "\"0x" << std::setw(16) << resp.getCas() << "\"";
    cas_string = ss.str();
    EXPECT_EQ(cas_string, resp.getValue());

    // Let's update the body version..
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "_sync.cas",
                  "\"Hell ain't such a bad place to be\"");
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // The macro should not have been expanded again...
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc_get("_sync.cas", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cas_string, resp.getValue());
    EXPECT_NE(first_cas, resp.getCas());
}

// Test that macro expansion only happens once if the value is replaced.
TEST_P(XattrTest, TestMacroExpansionOccursOnce) {
    getConnection().mutate(document, Vbid(0), MutationType::Set);

    createXattr("meta.cas", "\"${Mutation.CAS}\"", true);
    const auto mutation_cas = getXattr("meta.cas");
    EXPECT_NE("\"${Mutation.CAS}\"", mutation_cas.getValue())
            << "Macro expansion did not occur when requested";
    getConnection().mutate(document, Vbid(0), MutationType::Replace);
    EXPECT_EQ(mutation_cas, getXattr("meta.cas"))
            << "'meta.cas' should be unchanged when value replaced";
}

TEST_P(XattrTest, OperateOnDeletedItem) {
    getConnection().remove(name, Vbid(0));

    // let's add an attribute to the deleted document
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                       name,
                       "_sync.deleted",
                       "true",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P,
                       mcbp::subdoc::doc_flag::AccessDeleted);
    ASSERT_EQ(cb::mcbp::Status::SubdocSuccessDeleted, resp.getStatus());

    resp = subdoc_get("_sync.deleted",
                      SUBDOC_FLAG_XATTR_PATH,
                      mcbp::subdoc::doc_flag::AccessDeleted);
    ASSERT_EQ(cb::mcbp::Status::SubdocSuccessDeleted, resp.getStatus());
    EXPECT_EQ("true", resp.getValue());
}

TEST_P(XattrTest, MB_22318) {
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc", "{\"author\": \"Bart\"}"));
}

TEST_P(XattrTest, MB_22319) {
    // This is listed as working in the bug report
    EXPECT_EQ(uint8_t(cb::mcbp::Status::Success),
              uint8_t(xattr_upsert("doc.readcount", "0")));
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc.author", "\"jack\""));

    // The failing bits is:
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    "doc.readcount",
                    "1");
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    "doc.author",
                    "\"jones\"");

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotResponse resp;
    conn.recvResponse(resp);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
}


/*
 * The spec lists a table of the behavior when operating on a
 * full XATTR spec or if it is a partial XATTR spec.
 */

/**
 * Reads the value of the given XATTR.
 */
TEST_P(XattrTest, Get_FullXattrSpec) {
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc", "{\"author\": \"Bart\",\"rev\":0}"));

    auto response = subdoc_get("doc", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, response.getStatus());
    EXPECT_EQ("{\"author\": \"Bart\",\"rev\":0}", response.getValue());
}

/**
 * Reads the sub-part of the given XATTR.
 */
TEST_P(XattrTest, Get_PartialXattrSpec) {
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc", "{\"author\": \"Bart\",\"rev\":0}"));

    auto response = subdoc_get("doc.author", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, response.getStatus());
    EXPECT_EQ("\"Bart\"", response.getValue());
}

/**
 * Returns true if the given XATTR exists.
 */
TEST_P(XattrTest, Exists_FullXattrSpec) {
    // The document exists, but we should not have any xattr's
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocExists,
                       name,
                       "doc",
                       {},
                       SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());

    // Create the xattr
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc", "{\"author\": \"Bart\",\"rev\":0}"));

    // Now it should exist
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocExists,
                  name,
                  "doc",
                  {},
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
}

/**
 * Returns true if the given XATTR exists and the given sub-part
 * of the XATTR exists.
 */
TEST_P(XattrTest, Exists_PartialXattrSpec) {
    // The document exists, but we should not have any xattr's
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocExists,
                       name,
                       "doc",
                       {},
                       SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());

    // Create the xattr
    EXPECT_EQ(cb::mcbp::Status::Success,
              xattr_upsert("doc", "{\"author\": \"Bart\",\"rev\":0}"));

    // Now it should exist
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocExists,
                  name,
                  "doc.author",
                  {},
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // But we don't have one named _sync
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocExists,
                  name,
                  "_sync.cas",
                  {},
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());
}

/**
 * If XATTR specified by X-Key does not exist then create it with the
 * given value.
 * If XATTR already exists - fail with SUBDOC_PATH_EEXISTS
 */
TEST_P(XattrTest, DictAdd_FullXattrSpec) {
    // Adding it the first time should work
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                       name,
                       "doc",
                       "{\"author\": \"Bart\"}",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // Adding it the first time should work, second time we should get EEXISTS
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                  name,
                  "doc",
                  "{\"author\": \"Bart\"}",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEexists, resp.getStatus());
}

/**
 * Adds a dictionary element specified by the X-Path to the given X-Key.
 */
TEST_P(XattrTest, DictAdd_PartialXattrSpec) {
    // Adding it the first time should work
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                       name,
                       "doc.author",
                       "\"Bart\"",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // Adding it the first time should work, second time we should get EEXISTS
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                  name,
                  "doc.author",
                  "\"Bart\"",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEexists, resp.getStatus());
}

/**
 * Replaces the whole XATTR specified by X-Key with the given value if
 * the XATTR exists, or creates it with the given value.
 */
TEST_P(XattrTest, DictUpsert_FullXattrSpec) {
    // Adding it the first time should work
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "doc",
                       "{\"author\": \"Bart\"}",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // We should be able to update it...
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "doc",
                  "{\"author\": \"Jones\"}",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("doc.author", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("\"Jones\"", resp.getValue());
}

/**
 * Upserts a dictionary element specified by the X-Path to the given X-Key.
 */
TEST_P(XattrTest, DictUpsert_PartialXattrSpec) {
    // Adding it the first time should work
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "doc",
                       "{\"author\": \"Bart\"}",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // We should be able to update it...
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "doc.author",
                  "\"Jones\"",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("doc.author", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("\"Jones\"", resp.getValue());
}

/**
 * Deletes the whole XATTR specified by X-Key
 */
TEST_P(XattrTest, Delete_FullXattrSpec) {
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "doc",
                       "{\"author\": \"Bart\"}",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDelete,
                  name,
                  "doc",
                  {},
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // THe entire stuff should be gone
    resp = subdoc_get("doc", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());
}

/**
 * Deletes the sub-part of the XATTR specified by X-Key and X-Path
 */
TEST_P(XattrTest, Delete_PartialXattrSpec) {
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "doc",
                       "{\"author\":\"Bart\",\"ref\":0}",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDelete,
                  name,
                  "doc.ref",
                  {},
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // THe entire stuff should be gone
    resp = subdoc_get("doc", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("{\"author\":\"Bart\"}", resp.getValue());
}

/**
 * If the XATTR specified by X-Key exists, then replace the whole XATTR,
 * otherwise fail with SUBDOC_PATH_EEXISTS
 */
TEST_P(XattrTest, Replace_FullXattrSpec) {
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocReplace,
                       name,
                       "doc",
                       "{\"author\":\"Bart\",\"ref\":0}",
                       SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());

    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                  name,
                  "doc",
                  "{\"author\": \"Bart\"}",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc(cb::mcbp::ClientOpcode::SubdocReplace,
                  name,
                  "doc",
                  "{\"author\":\"Bart\",\"ref\":0}",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("doc", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("{\"author\":\"Bart\",\"ref\":0}", resp.getValue());
}

/**
 * Replaces the sub-part of the XATTR-specified by X-Key and X-path.
 */
TEST_P(XattrTest, Replace_PartialXattrSpec) {
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocReplace,
                       name,
                       "doc.author",
                       "\"Bart\"",
                       SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictAdd,
                  name,
                  "doc",
                  "{\"author\":\"Bart\",\"rev\":0}",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc(cb::mcbp::ClientOpcode::SubdocReplace,
                  name,
                  "doc.author",
                  "\"Jones\"",
                  SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc_get("doc", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("{\"author\":\"Jones\",\"rev\":0}", resp.getValue());
}

/**
 * Appends an array element to the root of the given XATTR.
 */
TEST_P(XattrTest, ArrayPushLast_FullXattrSpec) {
    doArrayPushLastTest("authors");
}

/**
 * Appends an array element specified by X-Path to the given X-Key.
 */
TEST_P(XattrTest, ArrayPushLast_PartialXattrSpec) {
    doArrayPushLastTest("doc.authors");
}

/**
 * Appends an array element to the root of the given XATTR.
 */
TEST_P(XattrTest, ArrayPushFirst_FullXattrSpec) {
    doArrayPushFirstTest("authors");
}

/**
 * Prepends an array element specified by X-Path to the given X-Key.
 */
TEST_P(XattrTest, ArrayPushFirst_PartialXattrSpec) {
    doArrayPushFirstTest("doc.authors");
}

/**
 * Inserts an array element specified by X-Path to the given X-Key.
 */
TEST_P(XattrTest, ArrayInsert_FullXattrSpec) {
    doArrayInsertTest("doc.");
    // It should also work for just "foo[0]"
    doArrayInsertTest("foo");
}

/**
 * Inserts an array element specified by X-Path to the given X-Key.
 */
TEST_P(XattrTest, ArrayInsert_PartialXattrSpec) {
    doArrayInsertTest("doc.authors");
}

/**
 * Adds an array element specified to the root of the given X-Key,
 * iff that element doesn't already exist in the root.
 */
TEST_P(XattrTest, ArrayAddUnique_FullXattrSpec) {
    doAddUniqueTest("doc");
}

/**
 * Adds an array element specified by X-Path to the given X-Key,
 * iff that element doesn't already exist in the array.
 */
TEST_P(XattrTest, ArrayAddUnique_PartialXattrSpec) {
    doAddUniqueTest("doc.authors");
}

/**
 * Increments/decrements the value at the root of the given X-Key
 */
TEST_P(XattrTest, Counter_FullXattrSpec) {
    doCounterTest("doc");
}

/**
 * Increments/decrements the value at the given X-Path of the given X-Key.
 */
TEST_P(XattrTest, Counter_PartialXattrSpec) {
    doCounterTest("doc.counter");
}

////////////////////////////////////////////////////////////////////////
//  Verify that I can't do subdoc ops if it's not enabled by hello
////////////////////////////////////////////////////////////////////////
TEST_P(XattrDisabledTest, VerifyNotEnabled) {
    auto& conn = getConnection();
    conn.setXattrSupport(false);

    // All of the subdoc commands end up using the same method
    // to validate the xattr portion of the command so we'll just
    // check one
    BinprotSubdocCommand cmd;
    cmd.setOp(cb::mcbp::ClientOpcode::SubdocDictAdd);
    cmd.setKey(name);
    cmd.setPath("_sync.deleted");
    cmd.setValue("true");
    cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    cmd.addDocFlags(mcbp::subdoc::doc_flag::AccessDeleted |
                    mcbp::subdoc::doc_flag::Mkdoc);
    conn.sendCommand(cmd);

    BinprotSubdocResponse resp;
    conn.recvResponse(resp);

    ASSERT_EQ(cb::mcbp::Status::NotSupported, resp.getStatus());
}

TEST_P(XattrTest, MB_22691) {
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "integer_extra",
                       "1",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "integer",
                  "2",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus())
            << to_string(resp.getStatus());
}

TEST_P(XattrTest, MB_23882_VirtualXattrs) {
    // Test to check that we can get both an xattr and the main body in
    // subdoc multi-lookup
    setBodyAndXattr(value, {{sysXattr, xattrVal}});

    // Sanity checks and setup done lets try the multi-lookup

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$document", SUBDOC_FLAG_XATTR_PATH);
    cmd.addGet("$document.CAS", SUBDOC_FLAG_XATTR_PATH);
    cmd.addGet("$document.foobar", SUBDOC_FLAG_XATTR_PATH);
    cmd.addGet("_sync.eg", SUBDOC_FLAG_XATTR_PATH);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);

    auto& results = multiResp.getResults();

    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailure, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, results[0].status);

    // Ensure that we found all we expected and they're of the correct type:
    auto json = nlohmann::json::parse(results[0].value);
    EXPECT_TRUE(json["CAS"].is_string());
    EXPECT_TRUE(json["vbucket_uuid"].is_string());
    EXPECT_TRUE(json["seqno"].is_string());
    EXPECT_TRUE(json["exptime"].is_number());
    EXPECT_TRUE(json["value_bytes"].is_number());
    EXPECT_TRUE(json["deleted"].is_boolean());
    EXPECT_TRUE(json["flags"].is_number());

    if (mcd_env->getTestBucket().supportsLastModifiedVattr()) {
        EXPECT_TRUE(json["last_modified"].is_string());
    }

    // Verify exptime is showing as 0 (document has no expiry)
    EXPECT_EQ(0, json["exptime"].get<int>());

    // Verify that the flags is correct
    EXPECT_EQ(0xcaffee, json["flags"].get<int>());

    // verify that the datatype is correctly encoded and contains
    // the correct bits
    auto datatype = json["datatype"];
    ASSERT_TRUE(datatype.is_array());
    bool found_xattr = false;
    bool found_json = false;

    for (const auto tag : datatype) {
        if (tag.get<std::string>() == "xattr") {
            found_xattr = true;
        } else if (tag.get<std::string>() == "json") {
            found_json = true;
        } else if (tag.get<std::string>() == "snappy") {
            // Not currently checked; default engine doesn't support
            // storing as Snappy (will inflate) so not trivial to assert
            // when this should be true.
        } else {
            FAIL() << "Unexpected datatype: " << tag.get<std::string>();
        }
    }
    EXPECT_TRUE(found_json);
    EXPECT_TRUE(found_xattr);

    // Verify that we got a partial from the second one
    EXPECT_EQ(cb::mcbp::Status::Success, results[1].status);
    const std::string cas{std::string{"\""} + json["CAS"].get<std::string>() +
                          "\""};
    json = nlohmann::json::parse(multiResp.getResults()[1].value);
    EXPECT_EQ(cas, json.dump());

    // The third one didn't exist
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, results[2].status);

    // Expect that we could find _sync.eg
    EXPECT_EQ(cb::mcbp::Status::Success, results[3].status);
    EXPECT_EQ("99", results[3].value);
}

TEST_P(XattrTest, MB_23882_VirtualXattrs_GetXattrAndBody) {
    // Test to check that we can get both an xattr and the main body in
    // subdoc multi-lookup
    setBodyAndXattr(value, {{sysXattr, xattrVal}});

    // Sanity checks and setup done lets try the multi-lookup

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$document.deleted", SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ("false", multiResp.getResults()[0].value);
    EXPECT_EQ(value, multiResp.getResults()[1].value);
}

TEST_P(XattrTest, MB_23882_VirtualXattrs_IsReadOnly) {
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P,
                    "$document.CAS",
                    "foo");
    cmd.addMutation(cb::mcbp::ClientOpcode::Set, SUBDOC_FLAG_NONE, "", value);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocXattrCantModifyVattr,
              multiResp.getStatus());
}

TEST_P(XattrTest, MB_23882_VirtualXattrs_UnknownVattr) {
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$documents", SUBDOC_FLAG_XATTR_PATH); // should be $document

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocXattrUnknownVattr, multiResp.getStatus());
}

TEST_P(XattrTest, MB_25786_XTOC_VattrAndBody) {
    verify_xtoc_user_system_xattr();
    auto& conn = getConnection();
    BinprotSubdocMultiLookupResponse multiResp;
    // Also check we can't use $XTOC if we can't read any xattrs
    conn.dropPrivilege(cb::rbac::Privilege::SystemXattrRead);
    conn.dropPrivilege(cb::rbac::Privilege::XattrRead);
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$XTOC", SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE);
    conn.sendCommand(cmd);
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailure, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Eaccess, multiResp.getResults()[0].status);
    EXPECT_EQ("", multiResp.getResults()[0].value);
}

TEST_P(XattrTest, MB_25786_XTOC_Vattr_XattrReadPrivOnly) {
    verify_xtoc_user_system_xattr();
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$XTOC", SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE);

    auto& conn = getConnection();
    BinprotSubdocMultiLookupResponse multiResp;

    conn.dropPrivilege(cb::rbac::Privilege::SystemXattrRead);
    multiResp.clear();
    conn.sendCommand(cmd);
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(R"(["userXattr"])", multiResp.getResults()[0].value);
}

TEST_P(XattrTest, MB_25786_XTOC_Vattr_XattrSystemReadPrivOnly) {
    verify_xtoc_user_system_xattr();
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$XTOC", SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get, SUBDOC_FLAG_NONE);

    auto& conn = getConnection();
    BinprotSubdocMultiLookupResponse multiResp;

    conn.dropPrivilege(cb::rbac::Privilege::XattrRead);
    multiResp.clear();
    conn.sendCommand(cmd);
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(R"(["_sync"])", multiResp.getResults()[0].value);
}

TEST_P(XattrTest, MB_25786_XTOC_VattrNoXattrs) {
    std::string value = R"({"Test":45})";
    Document document;
    document.info.cas = mcbp::cas::Wildcard;
    document.info.flags = 0xcaffee;
    document.info.id = name;
    document.value = value;
    getConnection().mutate(document, Vbid(0), MutationType::Set);
    auto doc = getConnection().get(name, Vbid(0));

    EXPECT_EQ(doc.value, document.value);

    auto resp = subdoc_get("$XTOC", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("[]", resp.getValue());
}

TEST_P(XattrTest, MB_25562_IncludeValueCrc32cInDocumentVAttr) {
    // I want to test that the expected document value checksum is
    // returned as part of the '$document' Virtual XAttr.

    // Create a docuement with a given value
    auto& connection = getConnection();
    Vbid vbid = Vbid(0);
    if (std::tr1::get<2>(GetParam()) == ClientJSONSupport::Yes) {
        document.info.datatype = cb::mcbp::Datatype::JSON;
        document.value = R"({"Test":45})";
    } else {
        document.info.datatype = cb::mcbp::Datatype::Raw;
        document.value = "raw value";
    }
    connection.mutate(document, vbid, MutationType::Set);
    EXPECT_EQ(document.value, connection.get(document.info.id, vbid).value);

    // Add an XAttr to the document.
    // We want to check that the checksum computed by the server takes in
    // input only the document value (XAttrs excluded)
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "userXattr",
                       R"({"a":1})",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc_get("userXattr", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(R"({"a":1})", resp.getValue());

    // Compute the expected value checksum
    auto _crc32c = crc32c(
            reinterpret_cast<const unsigned char*>(document.value.c_str()),
            document.value.size(),
            0 /*crc_in*/);
    auto expectedValueCrc32c = "\"" + cb::to_hex(_crc32c) + "\"";

    // Get and verify the actual value checksum
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(document.info.id);
    cmd.addGet("$document.value_crc32c", SUBDOC_FLAG_XATTR_PATH);
    BinprotSubdocMultiLookupResponse multiResp;
    connection.executeCommand(cmd, multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(expectedValueCrc32c, multiResp.getResults()[0].value);
}

TEST_P(XattrTest, MB_25562_StampValueCrc32cInUserXAttr) {
    // I want to test that the expansion of macro '${Mutation.value_crc32c}'
    // sets the correct value checksum into the given user XAttr

    // Store the macro and verify that it is not expanded without the
    // SUBDOC_FLAG_EXPAND_MACROS flag.
    // Note: as the document will contain an XAttr, we prove also that the
    // checksum computed by the server takes in input only the document
    // value (XAttrs excluded).
    auto resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                       name,
                       "_sync.value_crc32c",
                       "\"${Mutation.value_crc32c}\"",
                       SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    resp = subdoc_get("_sync.value_crc32c", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ("\"${Mutation.value_crc32c}\"", resp.getValue());

    // Now change the user xattr to macro expansion
    resp = subdoc(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                  name,
                  "_sync.value_crc32c",
                  "\"${Mutation.value_crc32c}\"",
                  SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_EXPAND_MACROS);
    EXPECT_EQ(cb::mcbp::Status::Success, resp.getStatus());

    // Compute the expected value_crc32c
    auto value = getConnection().get(name, Vbid(0)).value;
    auto _crc32c = crc32c(reinterpret_cast<const unsigned char*>(value.c_str()),
                          value.size(),
                          0 /*crc_in*/);
    auto expectedValueCrc32c = "\"" + cb::to_hex(_crc32c) + "\"";

    // Fetch the xattr and verify that the macro expanded to the
    // expected body checksum
    resp = subdoc_get("_sync.value_crc32c", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ(expectedValueCrc32c, resp.getValue());

    // Repeat the check fetching the entire '_sync' path. Differently from the
    // check above, this check exposed issues in macro padding.
    resp = subdoc_get("_sync", SUBDOC_FLAG_XATTR_PATH);
    ASSERT_EQ(cb::mcbp::Status::Success, resp.getStatus());
    EXPECT_EQ("{\"value_crc32c\":" + expectedValueCrc32c + "}",
              resp.getValue());
}

// Test that one can fetch both the body and an XATTR on a deleted document.
TEST_P(XattrTest, MB24152_GetXattrAndBodyDeleted) {
    setBodyAndXattr(value, {{sysXattr, xattrVal}});

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(xattrVal, multiResp.getResults()[0].value);
    EXPECT_EQ(value, multiResp.getResults()[1].value);
}

// Test that attempting to get an XATTR and a Body when the XATTR doesn't exist
// (partially) succeeds - the body is returned.
TEST_P(XattrTest, MB24152_GetXattrAndBodyWithoutXattr) {

    // Create a document without an XATTR.
    getConnection().store(name, Vbid(0), value);

    // Attempt to request both the body and a non-existent XATTR.
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailure, multiResp.getStatus());

    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent,
              multiResp.getResults()[0].status);
    EXPECT_EQ("", multiResp.getResults()[0].value);

    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[1].status);
    EXPECT_EQ(value, multiResp.getResults()[1].value);
}

// Test that attempting to get an XATTR and a Body when the doc is deleted and
// empty (partially) succeeds - the XATTR is returned.
TEST_P(XattrTest, MB24152_GetXattrAndBodyDeletedAndEmpty) {
    // Store a document with body+XATTR; then delete it (so the body
    // becomes empty).
    setBodyAndXattr(value, {{sysXattr, xattrVal}});
    getConnection().remove(name, Vbid(0));

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailureDeleted,
              multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(xattrVal, multiResp.getResults()[0].value);

    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent,
              multiResp.getResults()[1].status);
    EXPECT_EQ("", multiResp.getResults()[1].value);
}

// Test that attempting to get an XATTR and a Body when the body is non-JSON
// succeeds.
TEST_P(XattrTest, MB24152_GetXattrAndBodyNonJSON) {
    // Store a document with a non-JSON body + XATTR.
    value = "non-JSON value";
    setBodyAndXattr(value, {{sysXattr, xattrVal}});

    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addLookup("", cb::mcbp::ClientOpcode::Get);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(xattrVal, multiResp.getResults()[0].value);

    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[1].status);
    EXPECT_EQ(value, multiResp.getResults()[1].value);
}

// Test that a partial failure on a multi-lookup on a deleted document returns
// SUBDOC_MULTI_PATH_FAILURE_DELETED
TEST_P(XattrTest, MB23808_MultiPathFailureDeleted) {
    // Store an initial body+XATTR; then delete it.
    setBodyAndXattr(value, {{sysXattr, xattrVal}});
    getConnection().remove(name, Vbid(0));

    // Lookup two XATTRs - one which exists and one which doesn't.
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    cmd.addGet(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    cmd.addGet("_sync.non_existant", SUBDOC_FLAG_XATTR_PATH);

    auto& conn = getConnection();
    conn.sendCommand(cmd);

    // We expect to successfully access the first (existing) XATTR; but not
    // the second.
    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailureDeleted,
              multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getResults()[0].status);
    EXPECT_EQ(xattrVal, multiResp.getResults()[0].value);

    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent,
              multiResp.getResults()[1].status);
}

TEST_P(XattrTest, SetXattrAndDeleteBasic) {
    setBodyAndXattr(value, {{sysXattr, "55"}});
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Delete, SUBDOC_FLAG_NONE, "", "");
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());

    // Should now only be XATTR datatype
    auto meta = conn.getMeta(name, Vbid(0), GetMetaVersion::V2);
    EXPECT_EQ(cb::mcbp::Status::Success, meta.first);
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_XATTR, meta.second.datatype);
    // Should also be marked as deleted
    EXPECT_EQ(1, meta.second.deleted);

    auto resp = subdoc_get(sysXattr,
                           SUBDOC_FLAG_XATTR_PATH,
                           mcbp::subdoc::doc_flag::AccessDeleted);
    EXPECT_EQ(cb::mcbp::Status::SubdocSuccessDeleted, resp.getStatus());
    EXPECT_EQ(xattrVal, resp.getValue());

    // Check we can't access the deleted document
    resp = subdoc_get(sysXattr, SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::KeyEnoent, resp.getStatus());

    BinprotSubdocMultiLookupCommand getCmd;
    getCmd.setKey(name);
    getCmd.addLookup("", cb::mcbp::ClientOpcode::Get);
    conn.sendCommand(getCmd);

    BinprotSubdocMultiLookupResponse getResp;
    conn.recvResponse(getResp);
    EXPECT_EQ(cb::mcbp::Status::KeyEnoent, getResp.getStatus());

    // Worth noting the difference in the way it fails if AccessDeleted is set.
    getCmd.addDocFlag(mcbp::subdoc::doc_flag::AccessDeleted);
    conn.sendCommand(getCmd);
    conn.recvResponse(getResp);
    EXPECT_EQ(cb::mcbp::Status::SubdocMultiPathFailureDeleted,
              getResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent,
              getResp.getResults().at(0).status);
}

TEST_P(XattrTest, SetXattrAndDeleteCheckUserXattrsDeleted) {
    setBodyAndXattr(value, {{sysXattr, xattrVal}});
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    "userXattr",
                    "66");
    cmd.addMutation(cb::mcbp::ClientOpcode::Delete, SUBDOC_FLAG_NONE, "", "");
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());

    // Should now only be XATTR datatype
    auto meta = conn.getMeta(name, Vbid(0), GetMetaVersion::V2);
    EXPECT_EQ(cb::mcbp::Status::Success, meta.first);
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_XATTR, meta.second.datatype);
    // Should also be marked as deleted
    EXPECT_EQ(1, meta.second.deleted);

    auto resp = subdoc_get("userXattr", SUBDOC_FLAG_XATTR_PATH);
    EXPECT_EQ(cb::mcbp::Status::KeyEnoent, resp.getStatus());

    // The delete should delete user Xattrs as well as the body, leaving only
    // system Xattrs
    resp = subdoc_get("userXattr",
                      SUBDOC_FLAG_XATTR_PATH,
                      mcbp::subdoc::doc_flag::AccessDeleted);
    EXPECT_EQ(cb::mcbp::Status::SubdocPathEnoent, resp.getStatus());

    // System Xattr should still be there so lets check it
    resp = subdoc_get(sysXattr,
                      SUBDOC_FLAG_XATTR_PATH,
                      mcbp::subdoc::doc_flag::AccessDeleted);
    EXPECT_EQ(cb::mcbp::Status::SubdocSuccessDeleted, resp.getStatus());
    EXPECT_EQ(xattrVal, resp.getValue());
}

TEST_P(XattrTest, SetXattrAndDeleteJustUserXattrs) {
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    "userXattr",
                    "66");
    cmd.addMutation(cb::mcbp::ClientOpcode::Set,
                    SUBDOC_FLAG_NONE,
                    "",
                    "{\"Field\": 88}");
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());

    cmd.clearMutations();
    cmd.addMutation(cb::mcbp::ClientOpcode::Delete, SUBDOC_FLAG_NONE, "", "");
    conn.sendCommand(cmd);
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
}

TEST_P(XattrTest, TestXattrDeleteDatatypes) {
    // See MB-25422. We test to make sure that the datatype of a document is
    // correctly altered after a soft delete.
    setBodyAndXattr(value, {{sysXattr, "55"}});
    BinprotSubdocMultiMutationCommand cmd;
    cmd.setKey(name);
    cmd.addMutation(cb::mcbp::ClientOpcode::SubdocDictUpsert,
                    SUBDOC_FLAG_XATTR_PATH,
                    sysXattr,
                    xattrVal);
    cmd.addMutation(cb::mcbp::ClientOpcode::Delete, SUBDOC_FLAG_NONE, "", "");
    auto& conn = getConnection();
    conn.sendCommand(cmd);

    BinprotSubdocMultiMutationResponse multiResp;
    conn.recvResponse(multiResp);
    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());

    // Should now only be XATTR datatype
    auto meta = conn.getMeta(name, Vbid(0), GetMetaVersion::V2);
    EXPECT_EQ(cb::mcbp::Status::Success, meta.first);
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_XATTR, meta.second.datatype);
    // Should also be marked as deleted
    EXPECT_EQ(1, meta.second.deleted);
}

/**
 * Users xattrs should be stored inside the user data, which means
 * that if one tries to add xattrs to a document which is at the
 * max size you can't add any additional xattrs
 */
TEST_P(XattrTest, mb25928_UserCantExceedDocumentLimit) {
    if (!GetTestBucket().supportsPrivilegedBytes()) {
        return;
    }

    auto& conn = getConnection();

    std::string blob(GetTestBucket().getMaximumDocSize(), '\0');
    conn.store("mb25928", Vbid(0), std::move(blob));

    std::string value(300, 'a');
    value.front() = '"';
    value.back() = '"';

    BinprotSubdocCommand cmd;
    BinprotSubdocResponse resp;

    cmd.setOp(cb::mcbp::ClientOpcode::SubdocDictUpsert);
    cmd.setKey("mb25928");
    cmd.setPath("user.long_string");
    cmd.setValue(value);
    cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    cmd.addDocFlags(mcbp::subdoc::doc_flag::None);

    conn.executeCommand(cmd, resp);
    EXPECT_FALSE(resp.isSuccess());
    EXPECT_EQ(cb::mcbp::Status::E2big, resp.getStatus());
}

/**
 * System xattrs should be stored in a separate 1MB chunk (in addition to
 * the users normal document limit). Verify that we can add a system xattr
 * on a document which is at the max size
 */
TEST_P(XattrTest, mb25928_SystemCanExceedDocumentLimit) {
    if (!GetTestBucket().supportsPrivilegedBytes()) {
        return;
    }

    auto& conn = getConnection();

    std::string blob(GetTestBucket().getMaximumDocSize(), '\0');
    conn.store("mb25928", Vbid(0), std::move(blob));

    // Let it be almost 1MB (the internal length fields and keys
    // is accounted for in the system space
    std::string value(1024 * 1024 - 40, 'a');
    value.front() = '"';
    value.back() = '"';

    BinprotSubdocCommand cmd;
    BinprotSubdocResponse resp;

    cmd.setOp(cb::mcbp::ClientOpcode::SubdocDictUpsert);
    cmd.setKey("mb25928");
    cmd.setPath("_system.long_string");
    cmd.setValue(value);
    cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    cmd.addDocFlags(mcbp::subdoc::doc_flag::None);

    conn.executeCommand(cmd, resp);
    EXPECT_TRUE(resp.isSuccess())
                << "Expected to be able to store system xattrs";
}

/**
 * System xattrs should be stored in a separate 1MB chunk (in addition to
 * the users normal document limit). Verify that we can't add system xattrs
 * which exceeds this limit.
 */
TEST_P(XattrTest, mb25928_SystemCantExceedSystemLimit) {
    if (!GetTestBucket().supportsPrivilegedBytes()) {
        return;
    }

    auto& conn = getConnection();

    std::string blob(GetTestBucket().getMaximumDocSize(), '\0');
    conn.store("mb25928", Vbid(0), std::move(blob));

    std::string value(1024 * 1024, 'a');
    value.front() = '"';
    value.back() = '"';

    BinprotSubdocCommand cmd;
    BinprotSubdocResponse resp;

    cmd.setOp(cb::mcbp::ClientOpcode::SubdocDictUpsert);
    cmd.setKey("mb25928");
    cmd.setPath("_system.long_string");
    cmd.setValue(value);
    cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    cmd.addDocFlags(mcbp::subdoc::doc_flag::None);

    conn.executeCommand(cmd, resp);
    EXPECT_FALSE(resp.isSuccess());
    EXPECT_EQ(cb::mcbp::Status::E2big, resp.getStatus())
            << "The system space is max 1M";
}

// Test replacing a compressed/uncompressed value with an uncompressed
// value. XATTRs should be correctly merged.
TEST_P(XattrTest, MB_28524_TestReplaceWithXattrUncompressed) {
    doReplaceWithXattrTest(false);
}

// Test replacing a compressed/uncompressed value with a compressed
// value. XATTRs should be correctly merged.
TEST_P(XattrTest, MB_28524_TestReplaceWithXattrCompressed) {
    doReplaceWithXattrTest(true);
}
