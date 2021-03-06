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
#include "client_mcbp_commands.h"
#include <mcbp/mcbp.h>
#include <array>
#include <gsl/gsl>
#include "tracing/tracer.h"

void BinprotCommand::encode(std::vector<uint8_t>& buf) const {
    protocol_binary_request_header header;
    fillHeader(header);
    buf.insert(
            buf.end(), header.bytes, &header.bytes[0] + sizeof(header.bytes));
}

BinprotCommand::Encoded BinprotCommand::encode() const {
    Encoded bufs;
    encode(bufs.header);
    return bufs;
}

void BinprotCommand::fillHeader(protocol_binary_request_header& header,
                                size_t payload_len,
                                size_t extlen) const {
    header.request.magic = PROTOCOL_BINARY_REQ;
    header.request.setOpcode(opcode);
    header.request.keylen = htons(gsl::narrow<uint16_t>(key.size()));
    header.request.extlen = gsl::narrow<uint8_t>(extlen);
    header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
    header.request.vbucket = vbucket.hton();
    header.request.bodylen =
            htonl(gsl::narrow<uint32_t>(key.size() + extlen + payload_len));
    header.request.opaque = 0xdeadbeef;
    header.request.cas = cas;
}

void BinprotCommand::writeHeader(std::vector<uint8_t>& buf,
                                 size_t payload_len,
                                 size_t extlen) const {
    protocol_binary_request_header* hdr;
    buf.resize(sizeof(hdr->bytes));
    hdr = reinterpret_cast<protocol_binary_request_header*>(buf.data());
    fillHeader(*hdr, payload_len, extlen);
}

BinprotCommand& BinprotCommand::setKey(std::string key_) {
    key = std::move(key_);
    return *this;
}

BinprotCommand& BinprotCommand::setCas(uint64_t cas_) {
    cas = cas_;
    return *this;
}

BinprotCommand& BinprotCommand::setOp(cb::mcbp::ClientOpcode cmd_) {
    opcode = cmd_;
    return *this;
}

void BinprotCommand::clear() {
    opcode = cb::mcbp::ClientOpcode::Invalid;
    key.clear();
    cas = 0;
    vbucket = Vbid(0);
}

uint64_t BinprotCommand::getCas() const {
    return cas;
}

const std::string& BinprotCommand::getKey() const {
    return key;
}

cb::mcbp::ClientOpcode BinprotCommand::getOp() const {
    return opcode;
}

BinprotCommand& BinprotCommand::setVBucket(Vbid vbid) {
    vbucket = vbid;
    return *this;
}

BinprotCommand::Encoded::Encoded(BinprotCommand::Encoded&& other)
    : header(std::move(other.header)), bufs(std::move(other.bufs)) {
}

BinprotCommand::Encoded::Encoded() : header(), bufs() {
}

void BinprotCommand::ExpiryValue::assign(uint32_t value_) {
    value = value_;
    set = true;
}

void BinprotCommand::ExpiryValue::clear() {
    set = false;
}

bool BinprotCommand::ExpiryValue::isSet() const {
    return set;
}

uint32_t BinprotCommand::ExpiryValue::getValue() const {
    return value;
}

void BinprotGenericCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, value.size(), extras.size());
    buf.insert(buf.end(), extras.begin(), extras.end());
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), value.begin(), value.end());
}

BinprotGenericCommand::BinprotGenericCommand(cb::mcbp::ClientOpcode opcode,
                                             const std::string& key_,
                                             const std::string& value_)
    : BinprotCommandT() {
    setOp(opcode);
    setKey(key_);
    setValue(value_);
}

BinprotGenericCommand::BinprotGenericCommand(cb::mcbp::ClientOpcode opcode,
                                             const std::string& key_)
    : BinprotCommandT() {
    setOp(opcode);
    setKey(key_);
}

BinprotGenericCommand::BinprotGenericCommand(cb::mcbp::ClientOpcode opcode)
    : BinprotCommandT() {
    setOp(opcode);
}

BinprotGenericCommand::BinprotGenericCommand() : BinprotCommandT() {
}

BinprotGenericCommand& BinprotGenericCommand::setValue(std::string value_) {
    value = std::move(value_);
    return *this;
}

BinprotGenericCommand& BinprotGenericCommand::setExtras(
        const std::vector<uint8_t>& buf) {
    extras.assign(buf.begin(), buf.end());
    return *this;
}

void BinprotGenericCommand::clear() {
    BinprotCommand::clear();
    value.clear();
    extras.clear();
}

BinprotSubdocCommand::BinprotSubdocCommand(
        cb::mcbp::ClientOpcode cmd_,
        const std::string& key_,
        const std::string& path_,
        const std::string& value_,
        protocol_binary_subdoc_flag pathFlags_,
        mcbp::subdoc::doc_flag docFlags_,
        uint64_t cas_)
    : BinprotCommandT() {
    setOp(cmd_);
    setKey(key_);
    setPath(path_);
    setValue(value_);
    addPathFlags(pathFlags_);
    addDocFlags(docFlags_);
    setCas(cas_);
}

BinprotSubdocCommand& BinprotSubdocCommand::setPath(std::string path_) {
    if (path_.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::out_of_range("BinprotSubdocCommand::setPath: Path too big");
    }
    path = std::move(path_);
    return *this;
}

void BinprotSubdocCommand::encode(std::vector<uint8_t>& buf) const {
    if (key.empty()) {
        throw std::logic_error("BinprotSubdocCommand::encode: Missing a key");
    }

    protocol_binary_request_subdocument request;
    memset(&request, 0, sizeof(request));

    // Expiry (optional) is encoded in extras. Only include if non-zero or
    // if explicit encoding of zero was requested.
    const bool include_expiry = (expiry.getValue() != 0 || expiry.isSet());
    const bool include_doc_flags = !isNone(doc_flags);

    // Populate the header.
    const size_t extlen = sizeof(uint16_t) + // Path length
                          1 + // flags
                          (include_expiry ? sizeof(uint32_t) : 0) +
                          (include_doc_flags ? sizeof(uint8_t) : 0);

    fillHeader(request.message.header, path.size() + value.size(), extlen);

    // Add extras: pathlen, flags, optional expiry
    request.message.extras.pathlen = htons(gsl::narrow<uint16_t>(path.size()));
    request.message.extras.subdoc_flags = flags;
    buf.insert(buf.end(),
               request.bytes,
               &request.bytes[0] + sizeof(request.bytes));

    if (include_expiry) {
        // As expiry is optional (and immediately follows subdoc_flags,
        // i.e. unaligned) there's no field in the struct; so use low-level
        // memcpy to populate it.
        uint32_t encoded_expiry = htonl(expiry.getValue());
        char* expbuf = reinterpret_cast<char*>(&encoded_expiry);
        buf.insert(buf.end(), expbuf, expbuf + sizeof encoded_expiry);
    }

    if (include_doc_flags) {
        const uint8_t* doc_flag_ptr =
                reinterpret_cast<const uint8_t*>(&doc_flags);
        buf.insert(buf.end(), doc_flag_ptr, doc_flag_ptr + sizeof(uint8_t));
    }

    // Add Body: key; path; value if applicable.
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), path.begin(), path.end());
    buf.insert(buf.end(), value.begin(), value.end());
}
BinprotSubdocCommand::BinprotSubdocCommand() : BinprotCommandT() {
}
BinprotSubdocCommand::BinprotSubdocCommand(cb::mcbp::ClientOpcode cmd_) {
    setOp(cmd_);
}
BinprotSubdocCommand::BinprotSubdocCommand(cb::mcbp::ClientOpcode cmd_,
                                           const std::string& key_,
                                           const std::string& path_)
    : BinprotSubdocCommand(cmd_,
                           key_,
                           path_,
                           "",
                           SUBDOC_FLAG_NONE,
                           mcbp::subdoc::doc_flag::None,
                           0) {
}
BinprotSubdocCommand& BinprotSubdocCommand::setValue(std::string value_) {
    value = std::move(value_);
    return *this;
}
BinprotSubdocCommand& BinprotSubdocCommand::addPathFlags(
        protocol_binary_subdoc_flag flags_) {
    static protocol_binary_subdoc_flag validFlags = SUBDOC_FLAG_XATTR_PATH |
                                                    SUBDOC_FLAG_MKDIR_P |
                                                    SUBDOC_FLAG_EXPAND_MACROS;
    if ((flags_ & ~validFlags) == 0) {
        flags = flags | flags_;
    } else {
        throw std::invalid_argument("addPathFlags: flags_ (which is " +
                                    std::to_string(flags_) +
                                    ") is not a path flag");
    }
    return *this;
}
BinprotSubdocCommand& BinprotSubdocCommand::addDocFlags(
        mcbp::subdoc::doc_flag flags_) {
    static const mcbp::subdoc::doc_flag validFlags =
            mcbp::subdoc::doc_flag::Mkdoc |
            mcbp::subdoc::doc_flag::AccessDeleted | mcbp::subdoc::doc_flag::Add;
    if ((flags_ & ~validFlags) == mcbp::subdoc::doc_flag::None) {
        doc_flags = doc_flags | flags_;
    } else {
        throw std::invalid_argument("addDocFlags: flags_ (which is " +
                                    to_string(flags_) + ") is not a doc flag");
    }
    return *this;
}
BinprotSubdocCommand& BinprotSubdocCommand::setExpiry(uint32_t value_) {
    expiry.assign(value_);
    return *this;
}
const std::string& BinprotSubdocCommand::getPath() const {
    return path;
}
const std::string& BinprotSubdocCommand::getValue() const {
    return value;
}
protocol_binary_subdoc_flag BinprotSubdocCommand::getFlags() const {
    return flags;
}

bool BinprotResponse::isSuccess() const {
    return getStatus() == cb::mcbp::Status::Success;
}

void BinprotResponse::assign(std::vector<uint8_t>&& srcbuf) {
    payload = std::move(srcbuf);
}

boost::optional<std::chrono::microseconds> BinprotResponse::getTracingData()
        const {
    auto framingExtrasLen = getFramingExtraslen();
    if (framingExtrasLen == 0) {
        return boost::optional<std::chrono::microseconds>{};
    }
    const auto& framingExtras = getResponse().getFramingExtras();
    const auto& data = framingExtras.data();
    size_t offset = 0;

    // locate the tracing info
    while (offset < framingExtrasLen) {
        const uint8_t id = data[offset] & 0xF0;
        const uint8_t len = data[offset] & 0x0F;
        if (0 == id) {
            uint16_t micros = ntohs(
                    reinterpret_cast<const uint16_t*>(data + offset + 1)[0]);
            return cb::tracing::Tracer::decodeMicros(micros);
        }
        offset += 1 + len;
    }

    return boost::optional<std::chrono::microseconds>{};
}

cb::mcbp::ClientOpcode BinprotResponse::getOp() const {
    return cb::mcbp::ClientOpcode(getResponse().getClientOpcode());
}

cb::mcbp::Status BinprotResponse::getStatus() const {
    return getResponse().getStatus();
}

size_t BinprotResponse::getExtlen() const {
    return getResponse().getExtlen();
}

size_t BinprotResponse::getBodylen() const {
    return getResponse().getBodylen();
}

size_t BinprotResponse::getFramingExtraslen() const {
    return getResponse().getFramingExtraslen();
}

size_t BinprotResponse::getHeaderLen() {
    static const protocol_binary_request_header header{};
    return sizeof(header.bytes);
}

uint64_t BinprotResponse::getCas() const {
    return getResponse().cas;
}

protocol_binary_datatype_t BinprotResponse::getDatatype() const {
    return protocol_binary_datatype_t(getResponse().getDatatype());
}

const uint8_t* BinprotResponse::getPayload() const {
    return begin() + getHeaderLen();
}

cb::const_char_buffer BinprotResponse::getKey() const {
    const auto buf = getResponse().getKey();
    return {reinterpret_cast<const char*>(buf.data()), buf.size()};
}

std::string BinprotResponse::getKeyString() const {
    const auto buf = getKey();
    return {buf.data(), buf.size()};
}

cb::const_byte_buffer BinprotResponse::getData() const {
    return getResponse().getValue();
}

std::string BinprotResponse::getDataString() const {
    const auto buf = getData();
    return {reinterpret_cast<const char*>(buf.data()), buf.size()};
}

const cb::mcbp::Response& BinprotResponse::getResponse() const {
    return getHeader().getResponse();
}

void BinprotResponse::clear() {
    payload.clear();
}

const cb::mcbp::Header& BinprotResponse::getHeader() const {
    return *reinterpret_cast<const cb::mcbp::Header*>(payload.data());
}

const uint8_t* BinprotResponse::begin() const {
    return payload.data();
}

void BinprotSubdocResponse::assign(std::vector<uint8_t>&& srcbuf) {
    BinprotResponse::assign(std::move(srcbuf));
    if (getBodylen() - getExtlen() - getFramingExtraslen() > 0) {
        value.assign(payload.data() + sizeof(protocol_binary_response_header) +
                             getExtlen() + getFramingExtraslen(),
                     payload.data() + payload.size());
    }
}
const std::string& BinprotSubdocResponse::getValue() const {
    return value;
}
void BinprotSubdocResponse::clear() {
    BinprotResponse::clear();
    value.clear();
}
bool BinprotSubdocResponse::operator==(
        const BinprotSubdocResponse& other) const {
    bool rv = getStatus() == other.getStatus();

    if (getStatus() == cb::mcbp::Status::Success) {
        rv = getValue() == other.getValue();
    }
    return rv;
}

void BinprotSaslAuthCommand::encode(std::vector<uint8_t>& buf) const {
    if (key.empty()) {
        throw std::logic_error("BinprotSaslAuthCommand: Missing mechanism (setMechanism)");
    }

    writeHeader(buf, challenge.size(), 0);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), challenge.begin(), challenge.end());
}
void BinprotSaslAuthCommand::setMechanism(const std::string& mech_) {
    setKey(mech_);
}
void BinprotSaslAuthCommand::setChallenge(cb::const_char_buffer data) {
    challenge.assign(data.begin(), data.size());
}

void BinprotSaslStepCommand::encode(std::vector<uint8_t>& buf) const {
    if (key.empty()) {
        throw std::logic_error("BinprotSaslStepCommand::encode: Missing mechanism (setMechanism");
    }
    if (challenge.empty()) {
        throw std::logic_error("BinprotSaslStepCommand::encode: Missing challenge response");
    }

    writeHeader(buf, challenge.size(), 0);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), challenge.begin(), challenge.end());
}
void BinprotSaslStepCommand::setMechanism(const std::string& mech) {
    setKey(mech);
}
void BinprotSaslStepCommand::setChallenge(cb::const_char_buffer data) {
    challenge.assign(data.begin(), data.size());
}

void BinprotCreateBucketCommand::setConfig(const std::string& module,
                                           const std::string& config) {
    module_config.assign(module.begin(), module.end());
    module_config.push_back(0x00);
    module_config.insert(module_config.end(), config.begin(), config.end());
}

void BinprotCreateBucketCommand::encode(std::vector<uint8_t>& buf) const {
    if (module_config.empty()) {
        throw std::logic_error("BinprotCreateBucketCommand::encode: Missing bucket module and config");
    }
    writeHeader(buf, module_config.size(), 0);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), module_config.begin(), module_config.end());
}
BinprotCreateBucketCommand::BinprotCreateBucketCommand(const char* name) {
    setKey(name);
}

void BinprotGetCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 0);
    buf.insert(buf.end(), key.begin(), key.end());
}

void BinprotGetAndLockCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, sizeof(lock_timeout));
    protocol_binary_request_getl *req;
    buf.resize(sizeof(req->bytes));
    req = reinterpret_cast<protocol_binary_request_getl*>(buf.data());
    req->message.body.expiration = htonl(lock_timeout);
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotGetAndLockCommand::BinprotGetAndLockCommand()
    : BinprotCommandT(), lock_timeout(0) {
}
BinprotGetAndLockCommand& BinprotGetAndLockCommand::setLockTimeout(
        uint32_t timeout) {
    lock_timeout = timeout;
    return *this;
}

void BinprotGetAndTouchCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, sizeof(expirytime));
    protocol_binary_request_gat *req;
    buf.resize(sizeof(req->bytes));
    req = reinterpret_cast<protocol_binary_request_gat*>(buf.data());
    req->message.body.expiration = htonl(expirytime);
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotGetAndTouchCommand::BinprotGetAndTouchCommand()
    : BinprotCommandT(), expirytime(0) {
}
bool BinprotGetAndTouchCommand::isQuiet() const {
    return getOp() == cb::mcbp::ClientOpcode::Gatq;
}
BinprotGetAndTouchCommand& BinprotGetAndTouchCommand::setQuiet(bool quiet) {
    setOp(quiet ? cb::mcbp::ClientOpcode::Gatq : cb::mcbp::ClientOpcode::Gat);
    return *this;
}
BinprotGetAndTouchCommand& BinprotGetAndTouchCommand::setExpirytime(
        uint32_t timeout) {
    expirytime = timeout;
    return *this;
}

void BinprotTouchCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, sizeof(expirytime));
    protocol_binary_request_touch *req;
    buf.resize(sizeof(req->bytes));
    req = reinterpret_cast<protocol_binary_request_touch*>(buf.data());
    req->message.body.expiration = htonl(expirytime);
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotTouchCommand& BinprotTouchCommand::setExpirytime(uint32_t timeout) {
    expirytime = timeout;
    return *this;
}

void BinprotUnlockCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 0);
    buf.insert(buf.end(), key.begin(), key.end());
}

uint32_t BinprotGetResponse::getDocumentFlags() const {
    if (!isSuccess()) {
        return 0;
    }
    return ntohl(*reinterpret_cast<const uint32_t*>(getPayload()));
}

BinprotMutationCommand& BinprotMutationCommand::setMutationType(
    MutationType type) {
    switch (type) {
    case MutationType::Add:
        setOp(cb::mcbp::ClientOpcode::Add);
        return *this;
    case MutationType::Set:
        setOp(cb::mcbp::ClientOpcode::Set);
        return *this;
    case MutationType::Replace:
        setOp(cb::mcbp::ClientOpcode::Replace);
        return *this;
    case MutationType::Append:
        setOp(cb::mcbp::ClientOpcode::Append);
        return *this;
    case MutationType::Prepend:
        setOp(cb::mcbp::ClientOpcode::Prepend);
        return *this;
    }

    throw std::invalid_argument(
        "BinprotMutationCommand::setMutationType: Mutation type not supported: " +
        std::to_string(int(type)));
}

std::string to_string(MutationType type) {
    switch (type) {
    case MutationType::Add: return "ADD";
    case MutationType::Set: return "SET";
    case MutationType::Replace: return "REPLACE";
    case MutationType::Append: return "APPEND";
    case MutationType::Prepend: return "PREPEND";
    }

    return "to_string(MutationType type) Unknown type: " +
           std::to_string(int(type));
}

BinprotMutationCommand& BinprotMutationCommand::setDocumentInfo(
        const DocumentInfo& info) {
    if (!info.id.empty()) {
        setKey(info.id);
    }

    setDocumentFlags(info.flags);
    setCas(info.cas);
    setExpiry(info.expiration);

    datatype = uint8_t(info.datatype);
    return *this;
}

void BinprotMutationCommand::encodeHeader(std::vector<uint8_t>& buf) const {
    if (key.empty()) {
        throw std::invalid_argument("BinprotMutationCommand::encode: Key is missing!");
    }
    if (!value.empty() && !value_refs.empty()) {
        throw std::invalid_argument("BinprotMutationCommand::encode: Both value and value_refs have items!");
    }

    uint8_t extlen = 8;

    protocol_binary_request_header *header;
    buf.resize(sizeof(header->bytes));
    header = reinterpret_cast<protocol_binary_request_header*>(buf.data());

    if (getOp() == cb::mcbp::ClientOpcode::Append ||
        getOp() == cb::mcbp::ClientOpcode::Prepend) {
        if (expiry.getValue() != 0) {
            throw std::invalid_argument("BinprotMutationCommand::encode: Expiry invalid with append/prepend");
        }
        extlen = 0;
    }

    size_t value_size = value.size();
    for (const auto& vbuf : value_refs) {
        value_size += vbuf.size();
    }

    fillHeader(*header, value_size, extlen);
    header->request.datatype = datatype;

    if (extlen != 0) {
        // Write the extras:

        protocol_binary_request_set *req;
        buf.resize(sizeof(req->bytes));
        req = reinterpret_cast<protocol_binary_request_set*>(buf.data());

        req->message.body.expiration = htonl(expiry.getValue());
        req->message.body.flags = htonl(flags);
    }
}

void BinprotMutationCommand::encode(std::vector<uint8_t>& buf) const {
    encodeHeader(buf);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), value.begin(), value.end());
    for (const auto& vbuf : value_refs) {
        buf.insert(buf.end(), vbuf.begin(), vbuf.end());
    }
}

BinprotCommand::Encoded BinprotMutationCommand::encode() const {
    Encoded ret;
    auto& hdrbuf = ret.header;
    encodeHeader(hdrbuf);
    hdrbuf.insert(hdrbuf.end(), key.begin(), key.end());
    hdrbuf.insert(hdrbuf.end(), value.begin(), value.end());

    ret.bufs.assign(value_refs.begin(), value_refs.end());
    return ret;
}
BinprotMutationCommand& BinprotMutationCommand::setValue(
        std::vector<uint8_t>&& value_) {
    value = std::move(value_);
    return *this;
}
template <typename T>
BinprotMutationCommand& BinprotMutationCommand::setValue(const T& value_) {
    value.assign(value_.begin(), value_.end());
    return *this;
}
template <typename T>
BinprotMutationCommand& BinprotMutationCommand::setValueBuffers(const T& bufs) {
    value_refs.assign(bufs.begin(), bufs.end());
    return *this;
}
BinprotMutationCommand& BinprotMutationCommand::addValueBuffer(
        cb::const_byte_buffer buf) {
    value_refs.emplace_back(buf);
    return *this;
}
BinprotMutationCommand& BinprotMutationCommand::setDatatype(uint8_t datatype_) {
    datatype = datatype_;
    return *this;
}
BinprotMutationCommand& BinprotMutationCommand::setDatatype(
        cb::mcbp::Datatype datatype_) {
    return setDatatype(uint8_t(datatype_));
}
BinprotMutationCommand& BinprotMutationCommand::setDocumentFlags(
        uint32_t flags_) {
    flags = flags_;
    return *this;
}
BinprotMutationCommand& BinprotMutationCommand::setExpiry(uint32_t expiry_) {
    expiry.assign(expiry_);
    return *this;
}

void BinprotMutationResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));

    if (!isSuccess()) {
        // No point parsing the other info..
        return;
    }

    mutation_info.cas = getCas();
    mutation_info.size = 0; // TODO: what's this?

    if (getExtlen() == 0) {
        mutation_info.vbucketuuid = 0;
        mutation_info.seqno = 0;
    } else if (getExtlen() == 16) {
        auto const* bufs = reinterpret_cast<const uint64_t*>(
                getPayload() + getFramingExtraslen());
        mutation_info.vbucketuuid = ntohll(bufs[0]);
        mutation_info.seqno = ntohll(bufs[1]);
    } else {
        throw std::runtime_error("BinprotMutationResponse::assign: Bad extras length");
    }
}
const MutationInfo& BinprotMutationResponse::getMutationInfo() const {
    return mutation_info;
}

void BinprotHelloCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, features.size() * 2, 0);
    buf.insert(buf.end(), key.begin(), key.end());

    for (auto f : features) {
        uint16_t enc = htons(f);
        const char* p = reinterpret_cast<const char*>(&enc);
        buf.insert(buf.end(), p, p + 2);
    }
}
BinprotHelloCommand::BinprotHelloCommand(const std::string& client_id)
    : BinprotCommandT() {
    setKey(client_id);
}
BinprotHelloCommand& BinprotHelloCommand::enableFeature(
        cb::mcbp::Feature feature, bool enabled) {
    if (enabled) {
        features.insert(static_cast<uint8_t>(feature));
    } else {
        features.erase(static_cast<uint8_t>(feature));
    }
    return *this;
}

void BinprotHelloResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));

    if (isSuccess()) {
        // Ensure body length is even
        if (((getBodylen() - getFramingExtraslen()) & 1) != 0) {
            throw std::runtime_error(
                    "BinprotHelloResponse::assign: "
                    "Invalid response returned. "
                    "Uneven body length");
        }

        auto const* end =
                reinterpret_cast<const uint16_t*>(getPayload() + getBodylen());
        auto const* cur = reinterpret_cast<const uint16_t*>(
                begin() + getResponse().getValueOffset());

        for (; cur != end; ++cur) {
            features.push_back(cb::mcbp::Feature(htons(*cur)));
        }
    }
}
const std::vector<cb::mcbp::Feature>& BinprotHelloResponse::getFeatures()
        const {
    return features;
}

void BinprotIncrDecrCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 20);
    if (getOp() != cb::mcbp::ClientOpcode::Decrement &&
        getOp() != cb::mcbp::ClientOpcode::Increment) {
        throw std::invalid_argument(
                "BinprotIncrDecrCommand::encode: Invalid opcode. Need INCREMENT or DECREMENT");
    }

    // Write the delta
    for (auto n : std::array<uint64_t, 2>{{delta, initial}}) {
        uint64_t tmp = htonll(n);
        auto const* p = reinterpret_cast<const char*>(&tmp);
        buf.insert(buf.end(), p, p + 8);
    }

    uint32_t exptmp = htonl(expiry.getValue());
    auto const* p = reinterpret_cast<const char*>(&exptmp);
    buf.insert(buf.end(), p, p + 4);
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotIncrDecrCommand& BinprotIncrDecrCommand::setDelta(uint64_t delta_) {
    delta = delta_;
    return *this;
}
BinprotIncrDecrCommand& BinprotIncrDecrCommand::setInitialValue(
        uint64_t initial_) {
    initial = initial_;
    return *this;
}
BinprotIncrDecrCommand& BinprotIncrDecrCommand::setExpiry(uint32_t expiry_) {
    expiry.assign(expiry_);
    return *this;
}

void BinprotIncrDecrResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotMutationResponse::assign(std::move(buf));
    // Assign the value:
    if (isSuccess()) {
        value = htonll(*reinterpret_cast<const uint64_t*>(getData().data()));
    } else {
        value = 0;
    }
}
uint64_t BinprotIncrDecrResponse::getValue() const {
    return value;
}

void BinprotRemoveCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 0);
    buf.insert(buf.end(), key.begin(), key.end());
}

void BinprotGetErrorMapCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 2, 0);
    uint16_t encversion = htons(version);
    const char* p = reinterpret_cast<const char*>(&encversion);
    buf.insert(buf.end(), p, p + 2);
}
void BinprotGetErrorMapCommand::setVersion(uint16_t version_) {
    version = version_;
}

void BinprotSubdocMultiMutationCommand::encode(std::vector<uint8_t>& buf) const {
    // Calculate the size of the payload
    size_t total = 0;
    for (const auto& spec : specs) {
        // Accorrding to the spec the payload should be encoded as:
        //  1 @0         : Opcode
        //  1 @1         : Flags
        //  2 @2         : Path Length
        //  4 @4         : Value Length
        //  pathlen @8         : Path
        //  vallen @8+pathlen  : Value
        total += 1 + 1 + 2 + 4 + spec.path.size() + spec.value.size();
    }

    const uint8_t extlen =
            (expiry.isSet() ? 4 : 0) + (!isNone(docFlags) ? 1 : 0);
    writeHeader(buf, total, extlen);
    if (expiry.isSet()) {
        uint32_t expbuf = htonl(expiry.getValue());
        const char* p = reinterpret_cast<const char*>(&expbuf);
        buf.insert(buf.end(), p, p + 4);
    }
    if (!isNone(docFlags)) {
        const uint8_t* doc_flag_ptr =
                reinterpret_cast<const uint8_t*>(&docFlags);
        buf.insert(buf.end(), doc_flag_ptr, doc_flag_ptr + sizeof(uint8_t));
    }

    buf.insert(buf.end(), key.begin(), key.end());

    // Time to add the data:
    for (const auto& spec : specs) {
        buf.push_back(uint8_t(spec.opcode));
        buf.push_back(uint8_t(spec.flags));
        uint16_t pathlen = ntohs(gsl::narrow<uint16_t>(spec.path.size()));
        const char* p = reinterpret_cast<const char*>(&pathlen);
        buf.insert(buf.end(), p, p + 2);
        uint32_t vallen = ntohl(gsl::narrow<uint32_t>(spec.value.size()));
        p = reinterpret_cast<const char*>(&vallen);
        buf.insert(buf.end(), p, p + 4);
        buf.insert(buf.end(), spec.path.begin(), spec.path.end());
        buf.insert(buf.end(), spec.value.begin(), spec.value.end());
    }
}
BinprotSubdocMultiMutationCommand::BinprotSubdocMultiMutationCommand()
    : BinprotCommandT<BinprotSubdocMultiMutationCommand,
                      cb::mcbp::ClientOpcode::SubdocMultiMutation>(),
      docFlags(mcbp::subdoc::doc_flag::None) {
}
BinprotSubdocMultiMutationCommand&
BinprotSubdocMultiMutationCommand::addDocFlag(mcbp::subdoc::doc_flag docFlag) {
    static const mcbp::subdoc::doc_flag validFlags =
            mcbp::subdoc::doc_flag::Mkdoc |
            mcbp::subdoc::doc_flag::AccessDeleted | mcbp::subdoc::doc_flag::Add;
    if ((docFlag & ~validFlags) == mcbp::subdoc::doc_flag::None) {
        docFlags = docFlags | docFlag;
    } else {
        throw std::invalid_argument("addDocFlag: docFlag (Which is " +
                                    to_string(docFlag) + ") is not a doc flag");
    }
    return *this;
}
BinprotSubdocMultiMutationCommand&
BinprotSubdocMultiMutationCommand::addMutation(
        const BinprotSubdocMultiMutationCommand::MutationSpecifier& spec) {
    specs.push_back(spec);
    return *this;
}
BinprotSubdocMultiMutationCommand&
BinprotSubdocMultiMutationCommand::addMutation(
        cb::mcbp::ClientOpcode opcode,
        protocol_binary_subdoc_flag flags,
        const std::string& path,
        const std::string& value) {
    specs.emplace_back(MutationSpecifier{opcode, flags, path, value});
    return *this;
}
BinprotSubdocMultiMutationCommand& BinprotSubdocMultiMutationCommand::setExpiry(
        uint32_t expiry_) {
    expiry.assign(expiry_);
    return *this;
}
BinprotSubdocMultiMutationCommand::MutationSpecifier&
BinprotSubdocMultiMutationCommand::at(size_t index) {
    return specs.at(index);
}
BinprotSubdocMultiMutationCommand::MutationSpecifier&
        BinprotSubdocMultiMutationCommand::operator[](size_t index) {
    return specs[index];
}
bool BinprotSubdocMultiMutationCommand::empty() const {
    return specs.empty();
}
size_t BinprotSubdocMultiMutationCommand::size() const {
    return specs.size();
}
void BinprotSubdocMultiMutationCommand::clearMutations() {
    specs.clear();
}
void BinprotSubdocMultiMutationCommand::clearDocFlags() {
    docFlags = mcbp::subdoc::doc_flag::None;
}

void BinprotSubdocMultiMutationResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));
    switch (getStatus()) {
    case cb::mcbp::Status::Success:
    case cb::mcbp::Status::SubdocMultiPathFailure:
        break;
    default:
        return;
    }

    const uint8_t* bufcur = getData().data();
    const uint8_t* bufend = getData().data() + getData().size();

    // Result spec is:
    // 1@0          : Request Index
    // 2@1          : Status
    // 4@3          : Value length -- ONLY if status is success
    // $ValueLen@7  : Value

    while (bufcur < bufend) {
        uint8_t index = *bufcur;
        bufcur += 1;

        auto cur_status = cb::mcbp::Status(ntohs(*reinterpret_cast<const uint16_t*>(bufcur)));
        bufcur += 2;

        if (cur_status == cb::mcbp::Status::Success) {
            uint32_t cur_len =
                    ntohl(*reinterpret_cast<const uint32_t*>(bufcur));
            bufcur += 4;
            if (cur_len > bufend - bufcur) {
                throw std::runtime_error(
                        "BinprotSubdocMultiMutationResponse::assign(): "
                        "Invalid value length received");
            }
            results.emplace_back(MutationResult{
                    index,
                    cur_status,
                    std::string(reinterpret_cast<const char*>(bufcur),
                                cur_len)});
            bufcur += cur_len;
        } else {
            results.emplace_back(MutationResult{index, cur_status});
        }
    }
}
void BinprotSubdocMultiMutationResponse::clear() {
    BinprotResponse::clear();
    results.clear();
}
const std::vector<BinprotSubdocMultiMutationResponse::MutationResult>&
BinprotSubdocMultiMutationResponse::getResults() const {
    return results;
}

void BinprotSubdocMultiLookupCommand::encode(std::vector<uint8_t>& buf) const {
    size_t total = 0;
    // Payload is to be encoded as:
    // 1 @0         : Opcode
    // 1 @1         : Flags
    // 2 @2         : Path Length
    // $pathlen @4  : Path
    for (const auto& spec : specs) {
        total += 1 + 1 + 2 + spec.path.size();
    }

    const uint8_t extlen =
            (expiry.isSet() ? 4 : 0) + (!isNone(docFlags) ? 1 : 0);
    writeHeader(buf, total, extlen);

    // Note: Expiry isn't supported for multi lookups, but we specifically
    // test for it, and therefore allowed at the API level
    if (expiry.isSet()) {
        uint32_t expbuf = htonl(expiry.getValue());
        const char* p = reinterpret_cast<const char*>(&expbuf);
        buf.insert(buf.end(), p, p + 4);
    }
    if (!isNone(docFlags)) {
        const uint8_t* doc_flag_ptr =
                reinterpret_cast<const uint8_t*>(&docFlags);
        buf.insert(buf.end(), doc_flag_ptr, doc_flag_ptr + sizeof(uint8_t));
    }

    buf.insert(buf.end(), key.begin(), key.end());

    // Add the lookup specs themselves:
    for (const auto& spec : specs) {
        buf.push_back(uint8_t(spec.opcode));
        buf.push_back(uint8_t(spec.flags));

        uint16_t pathlen = ntohs(gsl::narrow<uint16_t>(spec.path.size()));
        const char* p = reinterpret_cast<const char*>(&pathlen);
        buf.insert(buf.end(), p, p + 2);
        buf.insert(buf.end(), spec.path.begin(), spec.path.end());
    }
}
BinprotSubdocMultiLookupCommand::BinprotSubdocMultiLookupCommand()
    : BinprotCommandT<BinprotSubdocMultiLookupCommand,
                      cb::mcbp::ClientOpcode::SubdocMultiLookup>(),
      docFlags(mcbp::subdoc::doc_flag::None) {
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addLookup(
        const BinprotSubdocMultiLookupCommand::LookupSpecifier& spec) {
    specs.push_back(spec);
    return *this;
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addLookup(
        const std::string& path,
        cb::mcbp::ClientOpcode opcode,
        protocol_binary_subdoc_flag flags) {
    return addLookup({opcode, flags, path});
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addGet(
        const std::string& path, protocol_binary_subdoc_flag flags) {
    return addLookup(path, cb::mcbp::ClientOpcode::SubdocGet, flags);
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addExists(
        const std::string& path, protocol_binary_subdoc_flag flags) {
    return addLookup(path, cb::mcbp::ClientOpcode::SubdocExists, flags);
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addGetcount(
        const std::string& path, protocol_binary_subdoc_flag flags) {
    return addLookup(path, cb::mcbp::ClientOpcode::SubdocGetCount, flags);
}
BinprotSubdocMultiLookupCommand& BinprotSubdocMultiLookupCommand::addDocFlag(
        mcbp::subdoc::doc_flag docFlag) {
    static const mcbp::subdoc::doc_flag validFlags =
            mcbp::subdoc::doc_flag::Mkdoc |
            mcbp::subdoc::doc_flag::AccessDeleted | mcbp::subdoc::doc_flag::Add;
    if ((docFlag & ~validFlags) == mcbp::subdoc::doc_flag::None) {
        docFlags = docFlags | docFlag;
    } else {
        throw std::invalid_argument("addDocFlag: docFlag (Which is " +
                                    to_string(docFlag) + ") is not a doc flag");
    }
    return *this;
}
void BinprotSubdocMultiLookupCommand::clearLookups() {
    specs.clear();
}
BinprotSubdocMultiLookupCommand::LookupSpecifier&
BinprotSubdocMultiLookupCommand::at(size_t index) {
    return specs.at(index);
}
BinprotSubdocMultiLookupCommand::LookupSpecifier&
        BinprotSubdocMultiLookupCommand::operator[](size_t index) {
    return specs[index];
}
bool BinprotSubdocMultiLookupCommand::empty() const {
    return specs.empty();
}
size_t BinprotSubdocMultiLookupCommand::size() const {
    return specs.size();
}
void BinprotSubdocMultiLookupCommand::clearDocFlags() {
    docFlags = mcbp::subdoc::doc_flag::None;
}
BinprotSubdocMultiLookupCommand&
BinprotSubdocMultiLookupCommand::setExpiry_Unsupported(uint32_t expiry_) {
    expiry.assign(expiry_);
    return *this;
}

void BinprotSubdocMultiLookupResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));
    // Check if this is a success - either full or partial.
    switch (getStatus()) {
    case cb::mcbp::Status::Success:
    case cb::mcbp::Status::SubdocMultiPathFailure:
    case cb::mcbp::Status::SubdocMultiPathFailureDeleted:
        break;
    default:
        return;
    }

    const uint8_t* bufcur = getData().data();
    const uint8_t* bufend = getData().data() + getData().size();

    // Result spec is:
    // 2@0          : Status
    // 4@0          : Value Length
    // $ValueLen@6  : Value

    while (bufcur < bufend) {
        uint16_t cur_status = ntohs(*reinterpret_cast<const uint16_t*>(bufcur));
        bufcur += 2;

        uint32_t cur_len = ntohl(*reinterpret_cast<const uint32_t*>(bufcur));
        bufcur += 4;

        results.emplace_back(LookupResult{
                cb::mcbp::Status(cur_status),
                std::string(reinterpret_cast<const char*>(bufcur), cur_len)});
        bufcur += cur_len;
    }
}
const std::vector<BinprotSubdocMultiLookupResponse::LookupResult>&
BinprotSubdocMultiLookupResponse::getResults() const {
    return results;
}
void BinprotSubdocMultiLookupResponse::clear() {
    BinprotResponse::clear();
    results.clear();
}

void BinprotGetCmdTimerCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 1);
    buf.push_back(std::underlying_type<cb::mcbp::ClientOpcode>::type(opcode));
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotGetCmdTimerCommand::BinprotGetCmdTimerCommand(
        cb::mcbp::ClientOpcode opcode)
    : BinprotCommandT(), opcode(opcode) {
}
BinprotGetCmdTimerCommand::BinprotGetCmdTimerCommand(
        const std::string& bucket, cb::mcbp::ClientOpcode opcode)
    : BinprotCommandT(), opcode(opcode) {
    setKey(bucket);
}
void BinprotGetCmdTimerCommand::setOpcode(cb::mcbp::ClientOpcode opcode) {
    BinprotGetCmdTimerCommand::opcode = opcode;
}
void BinprotGetCmdTimerCommand::setBucket(const std::string& bucket) {
    setKey(bucket);
}

void BinprotGetCmdTimerResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));
    if (isSuccess()) {
        timings.reset(cJSON_Parse(getDataString().c_str()));
        if (!timings) {
            throw std::runtime_error("BinprotGetCmdTimerResponse::assign: Invalid payload returned");
        }
    }
}
cJSON* BinprotGetCmdTimerResponse::getTimings() const {
    return timings.get();
}

void BinprotVerbosityCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 4);
    uint32_t value = ntohl(level);
    auto const* p = reinterpret_cast<const char*>(&value);
    buf.insert(buf.end(), p, p + 4);
}
void BinprotVerbosityCommand::setLevel(int level) {
    BinprotVerbosityCommand::level = level;
}

/**
 * Append a 16 bit integer to the buffer in network byte order
 *
 * @param buf the buffer to add the data to
 * @param value The value (in host local byteorder) to add.
 */
static void append(std::vector<uint8_t>& buf, uint16_t value) {
    uint16_t vallen = ntohs(value);
    auto p = reinterpret_cast<const char*>(&vallen);
    buf.insert(buf.end(), p, p + 2);
}

/**
 * Append a 32 bit integer to the buffer in network byte order
 *
 * @param buf the buffer to add the data to
 * @param value The value (in host local byteorder) to add.
 */
static void append(std::vector<uint8_t>& buf, uint32_t value) {
    uint32_t vallen = ntohl(value);
    auto p = reinterpret_cast<const char*>(&vallen);
    buf.insert(buf.end(), p, p + 4);
}

static uint8_t netToHost(uint8_t x) {
    return x;
}

static uint64_t netToHost(uint64_t x) {
    return ntohll(x);
}

static Vbid netToHost(Vbid x) {
    return x.ntoh();
}

/**
 * Extract the specified type from the buffer position. Returns an iterator
 * to the next element after the type extracted.
 */
template <typename T>
static std::vector<uint8_t>::iterator extract(
        std::vector<uint8_t>::iterator pos, T& value) {
    auto* p = reinterpret_cast<T*>(&*pos);
    value = netToHost(*p);
    return pos + sizeof(T);
}

/**
 * Append a 64 bit integer to the buffer in network byte order
 *
 * @param buf the buffer to add the data to
 * @param value The value (in host local byteorder) to add.
 */
void append(std::vector<uint8_t>& buf, uint64_t value) {
    uint64_t vallen = htonll(value);
    auto p = reinterpret_cast<const char*>(&vallen);
    buf.insert(buf.end(), p, p + 8);
}

void BinprotDcpOpenCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 8);
    append(buf, seqno);
    append(buf, flags);
    buf.insert(buf.end(), key.begin(), key.end());
}
BinprotDcpOpenCommand::BinprotDcpOpenCommand(const std::string& name,
                                             uint32_t seqno_,
                                             uint32_t flags_)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::DcpOpen, name, {}),
      seqno(seqno_),
      flags(flags_) {
}
BinprotDcpOpenCommand& BinprotDcpOpenCommand::makeProducer() {
    if (flags & DCP_OPEN_NOTIFIER) {
        throw std::invalid_argument(
                "BinprotDcpOpenCommand::makeProducer: a stream can't be both a "
                "consumer and producer");
    }
    flags |= DCP_OPEN_PRODUCER;
    return *this;
}
BinprotDcpOpenCommand& BinprotDcpOpenCommand::makeConsumer() {
    if (flags & DCP_OPEN_PRODUCER) {
        throw std::invalid_argument(
                "BinprotDcpOpenCommand::makeConsumer: a stream can't be both a "
                "consumer and producer");
    }
    flags |= DCP_OPEN_NOTIFIER;
    return *this;
}
BinprotDcpOpenCommand& BinprotDcpOpenCommand::makeIncludeXattr() {
    flags |= DCP_OPEN_INCLUDE_XATTRS;
    return *this;
}
BinprotDcpOpenCommand& BinprotDcpOpenCommand::makeNoValue() {
    flags |= DCP_OPEN_NO_VALUE;
    return *this;
}
BinprotDcpOpenCommand& BinprotDcpOpenCommand::setFlags(uint32_t flags) {
    BinprotDcpOpenCommand::flags = flags;
    return *this;
}

void BinprotDcpStreamRequestCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, 48);
    append(buf, dcp_flags);
    append(buf, dcp_reserved);
    append(buf, dcp_start_seqno);
    append(buf, dcp_end_seqno);
    append(buf, dcp_vbucket_uuid);
    append(buf, dcp_snap_start_seqno);
    append(buf, dcp_snap_end_seqno);
}
BinprotDcpStreamRequestCommand::BinprotDcpStreamRequestCommand()
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::DcpStreamReq, {}, {}),
      dcp_flags(0),
      dcp_reserved(0),
      dcp_start_seqno(std::numeric_limits<uint64_t>::min()),
      dcp_end_seqno(std::numeric_limits<uint64_t>::max()),
      dcp_vbucket_uuid(0),
      dcp_snap_start_seqno(std::numeric_limits<uint64_t>::min()),
      dcp_snap_end_seqno(std::numeric_limits<uint64_t>::max()) {
}
BinprotDcpStreamRequestCommand& BinprotDcpStreamRequestCommand::setDcpFlags(
        uint32_t value) {
    BinprotDcpStreamRequestCommand::dcp_flags = value;
    return *this;
}
BinprotDcpStreamRequestCommand& BinprotDcpStreamRequestCommand::setDcpReserved(
        uint32_t value) {
    BinprotDcpStreamRequestCommand::dcp_reserved = value;
    return *this;
}
BinprotDcpStreamRequestCommand&
BinprotDcpStreamRequestCommand::setDcpStartSeqno(uint64_t value) {
    BinprotDcpStreamRequestCommand::dcp_start_seqno = value;
    return *this;
}
BinprotDcpStreamRequestCommand& BinprotDcpStreamRequestCommand::setDcpEndSeqno(
        uint64_t value) {
    BinprotDcpStreamRequestCommand::dcp_end_seqno = value;
    return *this;
}
BinprotDcpStreamRequestCommand&
BinprotDcpStreamRequestCommand::setDcpVbucketUuid(uint64_t value) {
    BinprotDcpStreamRequestCommand::dcp_vbucket_uuid = value;
    return *this;
}
BinprotDcpStreamRequestCommand&
BinprotDcpStreamRequestCommand::setDcpSnapStartSeqno(uint64_t value) {
    BinprotDcpStreamRequestCommand::dcp_snap_start_seqno = value;
    return *this;
}
BinprotDcpStreamRequestCommand&
BinprotDcpStreamRequestCommand::setDcpSnapEndSeqno(uint64_t value) {
    BinprotDcpStreamRequestCommand::dcp_snap_end_seqno = value;
    return *this;
}

void BinprotDcpMutationCommand::reset(const std::vector<uint8_t>& packet) {
    clear();
    const auto* cmd = reinterpret_cast<const protocol_binary_request_dcp_mutation*>(packet.data());
    if (cmd->message.header.request.magic != uint8_t(PROTOCOL_BINARY_REQ)) {
        throw std::invalid_argument(
            "BinprotDcpMutationCommand::reset: packet is not a request");
    }

    by_seqno = ntohll(cmd->message.body.by_seqno);
    rev_seqno = ntohll(cmd->message.body.rev_seqno);
    flags = ntohl(cmd->message.body.flags);
    expiration = ntohl(cmd->message.body.expiration);
    lock_time = ntohl(cmd->message.body.lock_time);
    nmeta = ntohs(cmd->message.body.nmeta);
    nru = cmd->message.body.nru;

    setOp(cb::mcbp::ClientOpcode::DcpMutation);
    setVBucket(cmd->message.header.request.vbucket);
    setCas(cmd->message.header.request.cas);

    const char* ptr = reinterpret_cast<const char*>(cmd->bytes);
    // Non-collection aware DCP mutation, so pass false to getHeaderLength
    ptr += protocol_binary_request_dcp_mutation::getHeaderLength();

    const auto keylen = cmd->message.header.request.keylen;
    const auto bodylen = cmd->message.header.request.bodylen;
    const auto vallen = bodylen - keylen - cmd->message.header.request.extlen;

    setKey(std::string{ptr, keylen});
    ptr += keylen;
    setValue(std::string{ptr, vallen});
}

void BinprotDcpMutationCommand::encode(std::vector<uint8_t>& buf) const {
    throw std::runtime_error(
        "BinprotDcpMutationCommand::encode: not implemented");
}
BinprotDcpMutationCommand::BinprotDcpMutationCommand()
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::DcpMutation, {}, {}),
      by_seqno(0),
      rev_seqno(0),
      flags(0),
      expiration(0),
      lock_time(0),
      nmeta(0),
      nru(0) {
}
const std::string& BinprotDcpMutationCommand::getValue() const {
    return value;
}

void BinprotSetParamCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, value.size(), 4);
    append(buf, uint32_t(type));
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), value.begin(), value.end());
}
BinprotSetParamCommand::BinprotSetParamCommand(
        protocol_binary_engine_param_t type_,
        const std::string& key_,
        const std::string& value_)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::SetParam),
      type(type_),
      value(value_) {
    setKey(key_);
}

void BinprotSetWithMetaCommand::encode(std::vector<uint8_t>& buf) const {
    protocol_binary_request_header* hdr;
    buf.resize(sizeof(hdr->bytes));
    hdr = reinterpret_cast<protocol_binary_request_header*>(buf.data());

    size_t extlen = 24;

    if (options) {
        extlen += 4;
    }

    if (!meta.empty()) {
        extlen += 2;
    }

    fillHeader(*hdr, doc.value.size(), extlen);

    hdr->request.datatype = uint8_t(doc.info.datatype);
    append(buf, getFlags());
    append(buf, getExptime());
    append(buf, seqno);
    append(buf, getMetaCas());

    if (options) {
        append(buf, options);
    }

    if (!meta.empty()) {
        append(buf, uint16_t(htons(gsl::narrow<uint16_t>(meta.size()))));
    }

    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), doc.value.begin(), doc.value.end());
    buf.insert(buf.end(), meta.begin(), meta.end());
}
BinprotSetWithMetaCommand::BinprotSetWithMetaCommand(const Document& doc,
                                                     Vbid vbucket,
                                                     uint64_t operationCas,
                                                     uint64_t seqno,
                                                     uint32_t options,
                                                     std::vector<uint8_t> meta)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::SetWithMeta),
      doc(doc),
      seqno(seqno),
      operationCas(operationCas),
      options(options),
      meta(meta) {
    setVBucket(vbucket);
    setCas(operationCas);
    setKey(doc.info.id);
    setMeta(meta);
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setQuiet(bool quiet) {
    if (quiet) {
        setOp(cb::mcbp::ClientOpcode::SetqWithMeta);
    } else {
        setOp(cb::mcbp::ClientOpcode::SetWithMeta);
    }
    return *this;
}
uint32_t BinprotSetWithMetaCommand::getFlags() const {
    return doc.info.flags;
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setFlags(uint32_t flags) {
    doc.info.flags = flags;
    return *this;
}
uint32_t BinprotSetWithMetaCommand::getExptime() const {
    return doc.info.expiration;
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setExptime(
        uint32_t exptime) {
    doc.info.expiration = exptime;
    return *this;
}
uint64_t BinprotSetWithMetaCommand::getSeqno() const {
    return seqno;
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setSeqno(uint64_t seqno) {
    BinprotSetWithMetaCommand::seqno = seqno;
    return *this;
}
uint64_t BinprotSetWithMetaCommand::getMetaCas() const {
    return doc.info.cas;
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setMetaCas(uint64_t cas) {
    doc.info.cas = cas;
    return *this;
}
const std::vector<uint8_t>& BinprotSetWithMetaCommand::getMeta() {
    return meta;
}
BinprotSetWithMetaCommand& BinprotSetWithMetaCommand::setMeta(
        const std::vector<uint8_t>& meta) {
    std::copy(meta.begin(),
              meta.end(),
              std::back_inserter(BinprotSetWithMetaCommand::meta));
    return *this;
}

void BinprotSetControlTokenCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, 0, sizeof(token));
    append(buf, token);
}
BinprotSetControlTokenCommand::BinprotSetControlTokenCommand(uint64_t token_,
                                                             uint64_t oldtoken)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::SetCtrlToken),
      token(token_) {
    setCas(htonll(cas));
}

void BinprotSetClusterConfigCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, config.size(), 0);
    buf.insert(buf.end(), config.begin(), config.end());
}
BinprotSetClusterConfigCommand::BinprotSetClusterConfigCommand(
        uint64_t token_, const std::string& config_)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::SetClusterConfig),
      config(config_) {
    setCas(htonll(token_));
}

BinprotObserveSeqnoCommand::BinprotObserveSeqnoCommand(Vbid vbid, uint64_t uuid)
    : BinprotGenericCommand(cb::mcbp::ClientOpcode::ObserveSeqno), uuid(uuid) {
    setVBucket(vbid);
}

void BinprotObserveSeqnoCommand::encode(std::vector<uint8_t>& buf) const {
    writeHeader(buf, sizeof(uuid), 0);
    append(buf, uuid);
}

void BinprotObserveSeqnoResponse::assign(std::vector<uint8_t>&& buf) {
    BinprotResponse::assign(std::move(buf));
    if (!isSuccess()) {
        return;
    }

    if ((getBodylen() != 43) && (getBodylen() != 27)) {
        throw std::runtime_error(
                "BinprotObserveSeqnoResponse::assign: Invalid payload size - "
                "expected:43 or 27, actual:" +
                std::to_string(getBodylen()));
    }

    auto it = payload.begin() + getHeaderLen();
    it = extract(it, info.formatType);
    it = extract(it, info.vbId);
    it = extract(it, info.uuid);
    it = extract(it, info.lastPersistedSeqno);
    it = extract(it, info.currentSeqno);

    switch (info.formatType) {
    case 0:
        // No more fields for format 0.
        break;

    case 1:
        // Add in hard failover information
        it = extract(it, info.failoverUUID);
        it = extract(it, info.failoverSeqno);
        break;

    default:
        throw std::runtime_error(
                "BinprotObserveSeqnoResponse::assign: Unexpected formatType:" +
                std::to_string(info.formatType));
    }
}

BinprotUpdateUserPermissionsCommand::BinprotUpdateUserPermissionsCommand(
        std::string payload)
    : BinprotGenericCommand(
              cb::mcbp::ClientOpcode::UpdateExternalUserPermissions),
      payload(std::move(payload)) {
}

void BinprotUpdateUserPermissionsCommand::encode(
        std::vector<uint8_t>& buf) const {
    writeHeader(buf, payload.size(), 0);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), payload.begin(), payload.end());
}
