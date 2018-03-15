/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
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

#include "item_eviction.h"
#include "item.h"

#include <gsl/gsl>
#include <limits>

#include <hdr_histogram.h>

ItemEviction::ItemEviction() : requiredToUpdateInterval(learningPopulation) {
    struct hdr_histogram* hist;
    hdr_init(minFreqValue,
             maxFreqValue,
             3, // Number of significant figures
             &hist); // Pointer to initialise
    freqHistogram.reset(hist);
}

void ItemEviction::addValueToFreqHistogram(uint8_t v) {
    //  A hdr_histogram cannot store 0.  Therefore we add one so the
    // range moves from 0 -> 255 to 1 -> 256.
    int64_t vBiased = v + 1;
    hdr_record_value(freqHistogram.get(), vBiased);
}

uint64_t ItemEviction::getFreqHistogramValueCount() const {
    return freqHistogram->total_count;
}

void ItemEviction::reset() {
    hdr_reset(freqHistogram.get());
}

uint16_t ItemEviction::getFreqThreshold(double percentage) const {
    return gsl::narrow<uint16_t>(
            hdr_value_at_percentile(freqHistogram.get(), percentage));
}

uint8_t ItemEviction::convertFreqCountToNRUValue(uint8_t statCounter) {
    /*
     * The statstical counter has a range form 0 to 255, however the
     * increments are not linear - it gets more difficult to increment the
     * counter as its increases value.  Therefore incrementing from 0 to 1 is
     * much easier than incrementing from 254 to 255.
     *
     * Therefore when mapping to the 4 NRU values we do not simply want to
     * map 0-63 => 3, 64-127 => 2 etc.  Instead we want to reflect the bias
     * in the 4 NRU states.  Therefore we map as follows:
     * 0-3 => 3 (coldest), 4-31 => 2, 32->63 => 1, 64->255 => 0 (hottest),
     */
    if (statCounter >= 64) {
        return MIN_NRU_VALUE; /* 0 - the hottest */
    } else if (statCounter >= 32) {
        return 1;
    } else if (statCounter >= 4) {
        return INITIAL_NRU_VALUE; /* 2 */
    }
    return MAX_NRU_VALUE; /* 3 - the coldest */
}
