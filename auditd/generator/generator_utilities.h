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

#include <nlohmann/json_fwd.hpp>
#include <gsl/gsl>
#include <list>
#include <string>

class Module;
class Event;

/**
 * Is this build for enterprise edition?
 *
 * @return true when building EE, false for CE
 */
bool is_enterprise_edition();

/**
 * In order to allow making unit tests we want to be able to mock the
 * enterprise edition settings dynamically
 */
void set_enterprise_edition(bool enable);

/**
 * Load the requested file and parse it as JSON
 *
 * @param fname the name of the file
 * @return the cJSON representation of the file
 * @throws std::system_error if we fail to read the file
 *         std::runtime_error if we fail to parse the content of the file
 */
nlohmann::json load_file(const std::string& fname);

/**
 * Iterate over the module descriptor json and populate each entry
 * in the modules array into the provided modules list.
 *
 * @param ptr The JSON representation of the module description. See
 *            ../README.md for a description of the syntax
 * @param modules Where to store the list of all of the entries found
 * @param srcroot The source root to prepend to all of the paths in the spec
 * @param objroot The object root to prepend to all of the paths in the spec
 * @throws std::invalid_argument if the provided JSON is of an unexpected format
 */
void parse_module_descriptors(const nlohmann::json&,
                              std::list<std::unique_ptr<Module>>& modules,
                              const std::string& srcroot,
                              const std::string& objroot);

/**
 * Build the master event file
 *
 * @param modules The modules to include
 * @param output_file Where to store the result
 * @throws std::system_error if we fail to write the file
 */
void create_master_file(const std::list<std::unique_ptr<Module>>& modules,
                        const std::string& output_file);
