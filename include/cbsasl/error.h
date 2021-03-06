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

#include <cbsasl/visibility.h>
#include <string>

namespace cb {
namespace sasl {

/**
 * The error values used in CBSASL
 */
enum class Error {
    OK,
    CONTINUE,
    FAIL,
    BAD_PARAM,
    NO_MEM,
    NO_MECH,
    NO_USER,
    PASSWORD_ERROR,
    NO_RBAC_PROFILE,
    AUTH_PROVIDER_DIED
};
} // namespace sasl
} // namespace cb

CBSASL_PUBLIC_API
std::string to_string(cb::sasl::Error error);
