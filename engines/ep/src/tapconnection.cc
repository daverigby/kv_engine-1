/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include <limits>

#include <phosphor/phosphor.h>

#include "ep_engine.h"
#include "ep_time.h"
#include "tasks.h"
#define STATWRITER_NAMESPACE tap
#include "statwriter.h"
#undef STATWRITER_NAMESPACE
#include "tapconnection.h"
#include "vbucket.h"


std::atomic<uint64_t> ConnHandler::counter_(1);

class TapConfigChangeListener : public ValueChangedListener {
public:
    TapConfigChangeListener(TapConfig &c) : config(c) {
        // EMPTY
    }

    virtual void sizeValueChanged(const std::string &key, size_t value) {
        if (key.compare("tap_ack_grace_period") == 0) {
            config.setAckGracePeriod(value);
        } else if (key.compare("tap_ack_initial_sequence_number") == 0) {
            config.setAckInitialSequenceNumber(value);
        } else if (key.compare("tap_ack_interval") == 0) {
            config.setAckInterval(value);
        } else if (key.compare("tap_ack_window_size") == 0) {
            config.setAckWindowSize(value);
        } else if (key.compare("tap_bg_max_pending") == 0) {
            config.setBgMaxPending(value);
        } else if (key.compare("tap_backlog_limit") == 0) {
            config.setBackfillBacklogLimit(value);
        }
    }

    virtual void floatValueChanged(const std::string &key, float value) {
        if (key.compare("tap_backoff_period") == 0) {
            config.setBackoffSleepTime(value);
        } else if (key.compare("tap_requeue_sleep_time") == 0) {
            config.setRequeueSleepTime(value);
        } else if (key.compare("tap_backfill_resident") == 0) {
            config.setBackfillResidentThreshold(value);
        }
    }

private:
    TapConfig &config;
};

TapConfig::TapConfig(EventuallyPersistentEngine &e)
    : engine(e)
{
    Configuration &config = engine.getConfiguration();
    ackWindowSize = config.getTapAckWindowSize();
    ackInterval = config.getTapAckInterval();
    ackGracePeriod = config.getTapAckGracePeriod();
    ackInitialSequenceNumber = config.getTapAckInitialSequenceNumber();
    bgMaxPending = config.getTapBgMaxPending();
    backoffSleepTime = config.getTapBackoffPeriod();
    requeueSleepTime = config.getTapRequeueSleepTime();
    backfillBacklogLimit = config.getTapBacklogLimit();
    backfillResidentThreshold = config.getTapBackfillResident();
}

void TapConfig::addConfigChangeListener(EventuallyPersistentEngine &engine) {
    Configuration &configuration = engine.getConfiguration();
    configuration.addValueChangedListener("tap_ack_grace_period",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_initial_sequence_number",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_interval",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_window_size",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_bg_max_pending",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backoff_period",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_requeue_sleep_time",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backlog_limit",
                                          new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backfill_resident",
                                          new TapConfigChangeListener(engine.getTapConfig()));
}

ConnHandler::ConnHandler(EventuallyPersistentEngine& e, const void* c,
                         const std::string& n) :
    engine_(e),
    stats(engine_.getEpStats()),
    supportCheckpointSync_(false),
    name(n),
    cookie(const_cast<void*>(c)),
    reserved(false),
    connToken(gethrtime()),
    created(ep_current_time()),
    lastWalkTime(0),
    disconnect(false),
    connected(true),
    numDisconnects(0),
    expiryTime((rel_time_t)-1),
    supportAck(false) {}

ENGINE_ERROR_CODE ConnHandler::addStream(uint32_t opaque, uint16_t,
                                         uint32_t flags) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp add stream API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::closeStream(uint32_t opaque, uint16_t vbucket) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp close stream API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::streamEnd(uint32_t opaque, uint16_t vbucket,
                                         uint32_t flags) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp stream end API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::mutation(uint32_t opaque,
                                        const DocKey& key,
                                        cb::const_byte_buffer value,
                                        size_t priv_bytes,
                                        uint8_t datatype,
                                        uint64_t cas,
                                        uint16_t vbucket,
                                        uint32_t flags,
                                        uint64_t by_seqno,
                                        uint64_t rev_seqno,
                                        uint32_t expiration,
                                        uint32_t lock_time,
                                        cb::const_byte_buffer meta,
                                        uint8_t nru) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the mutation API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::deletion(uint32_t opaque,
                                        const DocKey& key,
                                        cb::const_byte_buffer value,
                                        size_t priv_bytes,
                                        uint8_t datatype,
                                        uint64_t cas,
                                        uint16_t vbucket,
                                        uint64_t by_seqno,
                                        uint64_t rev_seqno,
                                        cb::const_byte_buffer meta) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the deletion API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::expiration(uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          uint16_t vbucket,
                                          uint64_t by_seqno,
                                          uint64_t rev_seqno,
                                          cb::const_byte_buffer meta) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the expiration API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::snapshotMarker(uint32_t opaque,
                                              uint16_t vbucket,
                                              uint64_t start_seqno,
                                              uint64_t end_seqno,
                                              uint32_t flags)
{
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp snapshot marker API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::flushall(uint32_t opaque, uint16_t vbucket) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the flush API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::setVBucketState(uint32_t opaque,
                                               uint16_t vbucket,
                                               vbucket_state_t state) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the set vbucket state API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::streamRequest(uint32_t flags,
                                             uint32_t opaque,
                                             uint16_t vbucket,
                                             uint64_t start_seqno,
                                             uint64_t end_seqno,
                                             uint64_t vbucket_uuid,
                                             uint64_t snapStartSeqno,
                                             uint64_t snapEndSeqno,
                                             uint64_t *rollback_seqno,
                                             dcp_add_failover_log callback) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp stream request API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::getFailoverLog(uint32_t opaque, uint16_t vbucket,
                                              dcp_add_failover_log callback) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp get failover log API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::noop(uint32_t opaque) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the noop API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::bufferAcknowledgement(uint32_t opaque,
                                                     uint16_t vbucket,
                                                     uint32_t buffer_bytes) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the buffer acknowledgement API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::control(uint32_t opaque, const void* key,
                                       uint16_t nkey, const void* value,
                                       uint32_t nvalue) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the control API");
    return ENGINE_DISCONNECT;
}

ENGINE_ERROR_CODE ConnHandler::step(struct dcp_message_producers* producers) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp step API");
    return ENGINE_DISCONNECT;
}

bool ConnHandler::handleResponse(protocol_binary_response_header* resp) {
    logger.log(EXTENSION_LOG_WARNING, "Disconnecting - This connection doesn't "
        "support the dcp response handler API");
    return false;
}

ENGINE_ERROR_CODE ConnHandler::systemEvent(uint32_t opaque,
                                           uint16_t vbucket,
                                           mcbp::systemevent::id event,
                                           uint64_t bySeqno,
                                           cb::const_byte_buffer key,
                                           cb::const_byte_buffer eventData) {
    logger.log(EXTENSION_LOG_WARNING,
               "Disconnecting - This connection doesn't "
               "support the dcp system_event API");
    return ENGINE_DISCONNECT;
}

const Logger& ConnHandler::getLogger() const {
    return logger;
}

void ConnHandler::releaseReference(bool force)
{
    bool inverse = true;
    if (force || reserved.compare_exchange_strong(inverse, false)) {
        engine_.releaseCookie(cookie);
    }
}

void ConnHandler::setLastWalkTime() {
    lastWalkTime.store(ep_current_time());
}


/******************************* Consumer **************************************/
Consumer::Consumer(EventuallyPersistentEngine &engine, const void* cookie,
                   const std::string& name) :
    ConnHandler(engine, cookie, name),
    numDelete(0),
    numDeleteFailed(0),
    numFlush(0),
    numFlushFailed(0),
    numMutation(0),
    numMutationFailed(0),
    numOpaque(0),
    numOpaqueFailed(0),
    numVbucketSet(0),
    numVbucketSetFailed(0),
    numCheckpointStart(0),
    numCheckpointStartFailed(0),
    numCheckpointEnd(0),
    numCheckpointEndFailed(0),
    numUnknown(0) { }

void Consumer::addStats(ADD_STAT add_stat, const void *c) {
    ConnHandler::addStats(add_stat, c);
    addStat("num_delete", numDelete, add_stat, c);
    addStat("num_delete_failed", numDeleteFailed, add_stat, c);
    addStat("num_flush", numFlush, add_stat, c);
    addStat("num_flush_failed", numFlushFailed, add_stat, c);
    addStat("num_mutation", numMutation, add_stat, c);
    addStat("num_mutation_failed", numMutationFailed, add_stat, c);
    addStat("num_opaque", numOpaque, add_stat, c);
    addStat("num_opaque_failed", numOpaqueFailed, add_stat, c);
    addStat("num_vbucket_set", numVbucketSet, add_stat, c);
    addStat("num_vbucket_set_failed", numVbucketSetFailed, add_stat, c);
    addStat("num_checkpoint_start", numCheckpointStart, add_stat, c);
    addStat("num_checkpoint_start_failed", numCheckpointStartFailed, add_stat, c);
    addStat("num_checkpoint_end", numCheckpointEnd, add_stat, c);
    addStat("num_checkpoint_end_failed", numCheckpointEndFailed, add_stat, c);
    addStat("num_unknown", numUnknown, add_stat, c);
}

bool Consumer::isBackfillPhase(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine_.getKVBucket()->getVBuckets();
    VBucketPtr vb = vbuckets.getBucket(vbucket);
    if (vb && vb->isBackfillPhase()) {
        return true;
    }
    return false;
}

ENGINE_ERROR_CODE Consumer::setVBucketState(uint32_t opaque, uint16_t vbucket,
                                            vbucket_state_t state) {

    (void) opaque;

    if (!is_valid_vbucket_state_t(state)) {
        logger.log(EXTENSION_LOG_WARNING,
                   "Received an invalid vbucket state. Force disconnect");
        return ENGINE_DISCONNECT;
    }

    logger.log(EXTENSION_LOG_INFO,
               "Received TAP/DCP_VBUCKET_SET with vbucket %d and state \"%s\"",
               vbucket, VBucket::toString(state));

    // For TAP-based VBucket takeover, we should create a new VBucket UUID
    // to prevent any potential data loss after fully switching from TAP to
    // DCP. Please refer to https://issues.couchbase.com/browse/MB-15837 for
    // more details.
    return engine_.getKVBucket()->setVBucketState(vbucket, state, false);
}

void Consumer::processedEvent(uint16_t event, ENGINE_ERROR_CODE ret)
{
    switch (event) {
    case TAP_ACK:
        throw std::logic_error("Consumer::processedEvent: should never "
                "receive a TAP_ACK");
        break;

    case TAP_FLUSH:
        if (ret == ENGINE_SUCCESS) {
            ++numFlush;
        } else {
            ++numFlushFailed;
        }
        break;

    case TAP_DELETION:
        if (ret == ENGINE_SUCCESS) {
            ++numDelete;
        } else {
            ++numDeleteFailed;
        }
        break;

    case TAP_MUTATION:
        if (ret == ENGINE_SUCCESS) {
            ++numMutation;
        } else {
            ++numMutationFailed;
        }
        break;

    case TAP_OPAQUE:
        if (ret == ENGINE_SUCCESS) {
            ++numOpaque;
        } else {
            ++numOpaqueFailed;
        }
        break;

    case TAP_VBUCKET_SET:
        if (ret == ENGINE_SUCCESS) {
            ++numVbucketSet;
        } else {
            ++numVbucketSetFailed;
        }
        break;

    case TAP_CHECKPOINT_START:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointStart;
        } else {
            ++numCheckpointStartFailed;
        }
        break;

    case TAP_CHECKPOINT_END:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointEnd;
        } else {
            ++numCheckpointEndFailed;
        }
        break;

    default:
        ++numUnknown;
    }
}

void Consumer::checkVBOpenCheckpoint(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine_.getKVBucket()->getVBuckets();
    VBucketPtr vb = vbuckets.getBucket(vbucket);
    if (!vb || vb->getState() == vbucket_state_active) {
        return;
    }
    vb->checkpointManager.checkOpenCheckpoint(false, true);
}