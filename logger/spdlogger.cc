/* -*- MODE: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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

#include "custom_rotating_file_sink.h"

#include "logger.h"

#include <memcached/engine.h>
#include <memcached/extension.h>
#include <phosphor/phosphor.h>
#include <platform/processclock.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdio>

static SERVER_HANDLE_V1* sapi;
static EXTENSION_LOGGER_DESCRIPTOR descriptor;

static auto current_log_level = spdlog::level::warn;

/* Max suffix appended to the log file name.
 * The actual max no. of files is (max_files + 1), because the numbering starts
 * from the base file name (aka 0) eg. (file, file.1, ..., file.100)
 */
static size_t max_files = 100;

/* Custom log pattern which the loggers will use.
 * This pattern is duplicated for some test cases. If you need to update it,
 * please also update in all relevant places.
 * TODO: Remove the duplication in the future, by (maybe) moving
 *       the const to a header file.
 */
const std::string log_pattern{"%Y-%m-%dT%T.%fZ %l %v"};

static const spdlog::level::level_enum convertToSpdSeverity(
        EXTENSION_LOG_LEVEL sev) {
    using namespace spdlog::level;
    switch (sev) {
    case EXTENSION_LOG_DEBUG:
        return level_enum::debug;
    case EXTENSION_LOG_INFO:
        return level_enum::info;
    case EXTENSION_LOG_NOTICE:
        return level_enum::warn;
    case EXTENSION_LOG_WARNING:
        return level_enum::err;
    case EXTENSION_LOG_FATAL:
        return level_enum::critical;
    }
    throw std::invalid_argument("Unknown severity level");
}

/*
 * Instances of spdlog (async) file logger.
 * The files logger requires a rotating file sink which is manually configured
 * from the parsed settings.
 * The loggers act as a handle to the sinks. They do the processing of log
 * messages and send them to the sinks, which do the actual writing (to file,
 * to stream etc.)
 */
static std::shared_ptr<spdlog::logger> file_logger;

/* Returns the name of the file logger */
static const char* get_name() {
    return file_logger->name().c_str();
}

/* Retrieves a message, applies formatting and then logs it to stderr and
 * to file, according to the severity.
 */
static void log(EXTENSION_LOG_LEVEL mcd_severity,
                const void* client_cookie,
                const char* fmt,
                ...) {
    const auto severity = convertToSpdSeverity(mcd_severity);

    // Skip any processing if message wouldn't be logged anyway
    if (severity < current_log_level) {
        return;
    }

    // Retrieve formatted log message
    char msg[2048];
    int len;
    va_list va;
    va_start(va, fmt);
    len = vsnprintf(msg, 2048, fmt, va);
    va_end(va);

    // Something went wrong during formatting, so return
    if (len < 0) {
        return;
    }
    // len does not include '\0', hence >= and not >
    if (len >= int(sizeof(msg))) {
        // Crop message for logging
        const char cropped[] = " [cut]";
        snprintf(msg + (sizeof(msg) - sizeof(cropped)),
                 sizeof(cropped),
                 "%s",
                 cropped);
    } else {
        msg[len] = '\0';
    }

    file_logger->log(severity, msg);
}

/* (Synchronously) flushes  all the messages in the loggers' queue
 * and dereferences the loggers.
 */
static void logger_shutdown(bool force) {
    spdlog::drop(file_logger->name());

    file_logger.reset();
}

static void logger_flush() {
    file_logger->flush();
}

/* Updates current log level */
static void on_log_level(const void* cookie,
                         ENGINE_EVENT_TYPE type,
                         const void* event_data,
                         const void* cb_data) {
    if (sapi != NULL) {
        current_log_level = convertToSpdSeverity(sapi->log->get_level());
        file_logger->set_level(current_log_level);
    }
}

/* Initialises the loggers. Called if the logger configuration is
 * specified in a separate settings object.
 */
boost::optional<std::string> cb::logger::initialize(
        const Config& logger_settings, GET_SERVER_API get_server_api) {
    sapi = get_server_api();
    if (sapi == nullptr) {
        return boost::optional<std::string>{"Failed to get server API"};
    }

    auto fname = logger_settings.filename;
    auto buffersz = logger_settings.buffersize;
    auto cyclesz = logger_settings.cyclesize;
    auto sleeptime = logger_settings.sleeptime;

    if (getenv("CB_MINIMIZE_LOGGER_SLEEPTIME") != nullptr) {
        sleeptime = 1;
    }

    if (getenv("CB_MAXIMIZE_LOGGER_CYCLE_SIZE") != nullptr) {
        cyclesz = 1024 * 1024 * 1024; // use up to 1 GB log file size
    }

    if (getenv("CB_MAXIMIZE_LOGGER_BUFFER_SIZE") != nullptr) {
        buffersz = 8 * 1024 * 1024; // use an 8MB log buffer
    }

    try {
        auto sink = std::make_shared<spdlog::sinks::dist_sink_mt>();
        sink->add_sink(std::make_shared<custom_rotating_file_sink_mt>(
                fname, cyclesz, max_files, log_pattern));

        auto stderrsink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        stderrsink->set_level(spdlog::level::warn);
        sink->add_sink(stderrsink);

        file_logger =
                spdlog::create_async("spdlog_file_logger",
                                     sink,
                                     buffersz,
                                     spdlog::async_overflow_policy::block_retry,
                                     nullptr,
                                     std::chrono::seconds(sleeptime));
    } catch (const spdlog::spdlog_ex& ex) {
        std::string msg =
                std::string{"Log initialization failed: "} + ex.what();
        return boost::optional<std::string>{msg};
    }

    current_log_level = convertToSpdSeverity(sapi->log->get_level());

    file_logger->set_level(current_log_level);
    spdlog::set_pattern(log_pattern);

    descriptor.get_name = get_name;
    descriptor.log = log;
    descriptor.shutdown = logger_shutdown;
    descriptor.flush = logger_flush;

    if (!sapi->extension->register_extension(EXTENSION_LOGGER, &descriptor)) {
        return boost::optional<std::string>{"Failed to register logger"};
    }

    sapi->callback->register_callback(
            nullptr, ON_LOG_LEVEL, on_log_level, nullptr);
    return {};
}
