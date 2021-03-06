/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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

#pragma once

#include <gsl.h>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <string>

struct cJSON;

/**
 * The Event class represents the information needed for a single
 * audit event entry.
 */
class Event {
public:
    Event() = delete;

    /**
     * Construct and initialize a new Event structure based off the
     * provided JSON. See ../README.md for information about the
     * layout of the JSON element.
     *
     * @param entry
     * @throws std::runtime_error for errors accessing the expected
     *                            elements
     */
    explicit Event(const nlohmann::json& json);

    /// The identifier for this entry
    uint32_t id;
    /// The name of the entry
    std::string name;
    /// The full description of the entry
    std::string description;
    /// Set to true if this entry should be handled synchronously
    bool sync;
    /// Set to true if this entry is enabled (or should be dropped)
    bool enabled;
    /// Set to true if the user may enable filtering for the enry
    bool filtering_permitted;
    /// The textual representation of the JSON describing mandatory
    /// fields in the event (NOTE: this is currently not enforced
    /// by the audit daemon)
    std::string mandatory_fields;
    /// The textual representation of the JSON describing the optional
    /// fields in the event (NOTE: this is currently not enforced
    /// by the audit daemon)
    std::string optional_fields;
};
