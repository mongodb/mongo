/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <llvm/ADT/DenseMap.h>
#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <llvm/XRay/InstrumentationMap.h>
#include <llvm/XRay/Trace.h>

#include <fstream>
#include <sstream>

namespace xray_to_optrack {

/*
 * make_error --
 *     Helper for creating LLVM StringErrors.
 */
static llvm::Error
make_error(const std::string &msg)
{
    return llvm::make_error<llvm::StringError>(msg, llvm::inconvertibleErrorCode());
}

/*
 * xray_to_optrack_record_type --
 *     Convert from an XRay log record type to an OpTrack log record type.
 */
static llvm::Expected<int>
xray_to_optrack_record_type(const llvm::xray::RecordTypes record_type)
{
    switch (record_type) {
    case llvm::xray::RecordTypes::ENTER:
        return 0;
    case llvm::xray::RecordTypes::EXIT:
    case llvm::xray::RecordTypes::TAIL_EXIT:
        return 1;
    default: {
        std::stringstream ss;
        ss << "Unexpected record type \"" << static_cast<unsigned>(record_type) << "\"";
        return make_error(ss.str());
    }
    }
}

/*
 * write_optrack_record --
 *     Write an OpTrack record to a log file.
 */
static void
write_optrack_record(std::ofstream &os, int record_type, const std::string &func_name, uint64_t tsc)
{
    os << record_type << " " << func_name << " " << tsc << "\n";
}

/*
 * symbolize_func_id --
 *     Symbolize the full function name for a given XRay function id.
 */
static llvm::Expected<std::string>
symbolize_func_id(uint32_t func_id, const std::string &instr_map,
  llvm::symbolize::LLVMSymbolizer &symbolizer,
  const std::unordered_map<int32_t, uint64_t> &address_map,
  llvm::DenseMap<uint32_t, std::string> &cache)
{
    const auto cache_iter = cache.find(func_id);
    if (cache_iter != cache.end())
        return cache_iter->second;
    const auto iter = address_map.find(func_id);
    if (iter == address_map.end()) {
        std::stringstream ss;
        ss << "Found function id \"" << func_id << "\" without a corresponding address";
        return make_error(ss.str());
    }
    auto res = symbolizer.symbolizeCode(instr_map, {iter->second, llvm::object::SectionedAddress::UndefSection});
    if (!res)
        return res.takeError();
    if (res->FunctionName == "<invalid>") {
        std::stringstream ss;
        ss << "Could not symbolize id \"" << func_id << "\", address \"" << iter->second << "\"";
        return make_error(ss.str());
    }
    cache.try_emplace(func_id, res->FunctionName);
    return res->FunctionName;
}

/*
 * generate_optrack_log_name --
 *     Create a filename for an OpTrack log given its process id and thread id. The filename is in
 *     the format "optrack_<pid>_<tid>".
 */
static std::string
generate_optrack_log_name(uint32_t process_id, uint32_t thread_id)
{
    std::stringstream ss;
    ss << "optrack_" << process_id << "_" << thread_id;
    return ss.str();
}

/*
 * xray_to_optrack --
 *     Write an OpTrack log for each thread given an XRay log and instrumentation map.
 */
static llvm::Error
xray_to_optrack(const std::string &instr_map, const std::string &input)
{
    auto map = llvm::xray::loadInstrumentationMap(instr_map);
    if (!map)
        return map.takeError();
    auto trace = llvm::xray::loadTraceFile(input);
    if (!trace)
        return trace.takeError();
    llvm::symbolize::LLVMSymbolizer symbolizer;
    llvm::DenseMap<uint32_t, std::string> cache;
    llvm::DenseMap<uint32_t, std::ofstream> files;
    for (const llvm::xray::XRayRecord &record : *trace) {
        auto file_iter = files.find(record.TId);
        if (file_iter == files.end()) {
            std::ofstream file(generate_optrack_log_name(record.PId, record.TId));
            if (!file) {
                std::stringstream ss;
                ss << "Failed to write OpTrack log for process id \"" << record.PId
                   << "\", thread id \"" << record.TId << "\"";
                return make_error(ss.str());
            }
            auto res = files.try_emplace(record.TId, std::move(file));
            file_iter = res.first;
        }
        assert(file_iter != files.end());
        auto record_type = xray_to_optrack_record_type(record.Type);
        if (!record_type)
            return record_type.takeError();
        auto func_name = symbolize_func_id(
          record.FuncId, instr_map, symbolizer, map->getFunctionAddresses(), cache);
        if (!func_name)
            return func_name.takeError();
        write_optrack_record(file_iter->second, *record_type, *func_name, record.TSC);
    }
    return llvm::Error::success();
}

} // namespace xray_to_optrack

int
main(int argc, char **argv)
{
    if (argc != 3) {
        llvm::errs() << "Usage: xray_to_optrack <instr_map> <xray_log>\n";
        return EXIT_FAILURE;
    }
    auto error = xray_to_optrack::xray_to_optrack(argv[1], argv[2]);
    if (error) {
        llvm::errs() << "Error: " << error << ".\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
