#include "cache/dumper.hpp"

#include <fmt/compile.h>
#include <fmt/format.h>

#include <algorithm>
#include <boost/regex.hpp>
#include <fs/read.hpp>
#include <fs/write.hpp>
#include <utils/assert.hpp>
#include <utils/async.hpp>
#include <utils/datetime.hpp>
#include <utils/from_string.hpp>

namespace cache {

namespace {

const std::string kTimeZone = "UTC";

}  // namespace

Dumper::Dumper(CacheConfigStatic&& config,
               engine::TaskProcessor& fs_task_processor,
               std::string_view cache_name)
    : config_(std::move(config)),
      fs_task_processor_(fs_task_processor),
      cache_name_(cache_name),
      filename_regex_(GenerateFilenameRegex(FileFormatType::kNormal)),
      tmp_filename_regex_(GenerateFilenameRegex(FileFormatType::kTmp)) {}

bool Dumper::WriteNewDump(DumpContents dump) {
  const auto dump_size = dump.contents.size();
  const auto config = config_.Read();

  const std::string dump_path = GenerateDumpPath(dump.update_time, *config);

  if (fs::FileExists(fs_task_processor_, dump_path)) {
    LOG_ERROR() << "Could not dump cache " << cache_name_ << " to \""
                << dump_path << "\" file already exists";
    return false;
  }

  using perms = boost::filesystem::perms;
  const auto kPerms = perms::owner_read | perms::owner_write;

  try {
    fs::RewriteFileContentsAtomically(fs_task_processor_, dump_path,
                                      std::move(dump.contents), kPerms);
    LOG_INFO() << "Successfully dumped " << cache_name_ << " to \"" << dump_path
               << "\" (" << dump_size << " bytes total)";
    return true;
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while trying to dump cache " << cache_name_
                << " to \"" << dump_path << "\". Cause: " << ex;
    return false;
  }
}

std::optional<DumpContents> Dumper::ReadLatestDump() {
  const auto config = config_.Read();

  try {
    std::optional<std::string> filename = GetLatestDumpName(*config);
    if (!filename) {
      LOG_INFO() << "No usable cache dumps found for cache " << cache_name_;
      return std::nullopt;
    }

    const std::string dump_path = FilenameToPath(*filename, *config);
    LOG_DEBUG() << "A usable cache dump found for cache " << cache_name_
                << ": \"" << dump_path << "\"";

    std::string contents = fs::ReadFileContents(fs_task_processor_, dump_path);
    return DumpContents{std::move(contents),
                        ParseDumpName(std::move(*filename))->update_time};
  } catch (const std::exception& ex) {
    LOG_ERROR()
        << "Error while trying to read the contents of cache dump for cache "
        << cache_name_ << ". Cause: " << ex;
    return std::nullopt;
  }
}

bool Dumper::BumpDumpTime(TimePoint old_update_time,
                          TimePoint new_update_time) {
  UASSERT(old_update_time <= new_update_time);
  const auto config = config_.Read();

  const std::string old_name = GenerateDumpPath(old_update_time, *config);
  const std::string new_name = GenerateDumpPath(new_update_time, *config);

  try {
    if (!fs::FileExists(fs_task_processor_, old_name)) {
      LOG_WARNING()
          << "The previous cache dump \"" << old_name << "\" of cache "
          << cache_name_
          << " has suddenly disappeared. A new cache dump will be created.";
      return false;
    }
    fs::Rename(fs_task_processor_, old_name, new_name);
    LOG_INFO() << "Renamed cache dump \"" << old_name << "\" of cache "
               << cache_name_ << " to \"" << new_name << "\"";
    return true;
  } catch (const boost::filesystem::filesystem_error& ex) {
    LOG_ERROR() << "Error while trying to rename cache dump \"" << old_name
                << "\" of cache " << cache_name_ << " to \"" << new_name
                << "\". Reason: " << ex;
    return false;
  }
}

void Dumper::Cleanup() {
  const auto config = config_.Read();
  utils::Async(fs_task_processor_, "cache-dumper",
               [this, &config] { CleanupBlocking(*config); })
      .Get();
  config_.Cleanup();
}

void Dumper::SetConfig(const CacheConfigStatic& config) {
  config_.Assign(config);
}

std::optional<Dumper::ParsedDumpName> Dumper::ParseDumpName(
    std::string filename) const {
  boost::smatch regex;
  if (boost::regex_match(filename, regex, filename_regex_)) {
    UASSERT_MSG(regex.size() == 3,
                fmt::format("Incorrect sub-match count: {} for filename {}",
                            regex.size(), filename));

    try {
      const auto date = utils::datetime::Stringtime(regex[1].str(), kTimeZone,
                                                    kDumpFilenameDateFormat);
      const auto version = utils::FromString<uint64_t>(regex[2].str());
      return ParsedDumpName{std::move(filename), Round(date), version};
    } catch (const std::exception& ex) {
      LOG_WARNING() << "A filename looks like a cache dump of cache "
                    << cache_name_ << ", but it is not: \"" << filename
                    << "\". Reason: " << ex;
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> Dumper::GetLatestDumpNameBlocking(
    const CacheConfigStatic& config) const {
  const auto min_update_time = MinAcceptableUpdateTime(config);
  std::optional<ParsedDumpName> best_dump;

  try {
    for (const auto& file :
         boost::filesystem::directory_iterator{config.dump_directory}) {
      if (!boost::filesystem::is_regular_file(file.status())) {
        continue;
      }

      auto curr_dump = ParseDumpName(file.path().filename().string());
      if (!curr_dump) continue;

      if (curr_dump->format_version != config.dump_format_version) {
        LOG_DEBUG() << "Ignoring cache dump \"" << curr_dump->filename
                    << "\", because its format version ("
                    << curr_dump->format_version << ") != current version ("
                    << config.dump_format_version << ")";
        continue;
      }

      if (curr_dump->update_time < min_update_time && config.max_dump_age) {
        LOG_DEBUG() << "Ignoring cache dump \"" << curr_dump->filename
                    << "\", because its age is greater than the maximum "
                       "allowed cache dump age ("
                    << config.max_dump_age->count() << "ms)";
        continue;
      }

      if (!best_dump || curr_dump->update_time > best_dump->update_time) {
        best_dump = std::move(curr_dump);
      }
    }
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while trying to fetch cache dumps for cache "
                << cache_name_ << ". Cause: " << ex;
    // proceed to return best_dump
  }

  return best_dump ? std::optional{std::move(best_dump->filename)}
                   : std::nullopt;
}

std::optional<std::string> Dumper::GetLatestDumpName(
    const CacheConfigStatic& config) const {
  return utils::Async(
             fs_task_processor_, "cache-dumper",
             [this, &config] { return GetLatestDumpNameBlocking(config); })
      .Get();
}

void Dumper::CleanupBlocking(const CacheConfigStatic& config) {
  const auto min_update_time = MinAcceptableUpdateTime(config);
  std::vector<ParsedDumpName> dumps;

  try {
    for (const auto& file :
         boost::filesystem::directory_iterator{config.dump_directory}) {
      if (!boost::filesystem::is_regular_file(file.status())) {
        continue;
      }

      std::string filename = file.path().filename().string();

      boost::smatch what;
      if (boost::regex_match(filename, what, tmp_filename_regex_)) {
        LOG_DEBUG() << "Removing a leftover tmp file \"" << file.path().string()
                    << "\"";
        boost::filesystem::remove(file);
        continue;
      }

      auto dump = ParseDumpName(std::move(filename));
      if (!dump) continue;

      if (dump->format_version < config.dump_format_version ||
          dump->update_time < min_update_time) {
        LOG_DEBUG() << "Removing an expired dump \"" << file.path().string()
                    << "\" for cache " << cache_name_;
        boost::filesystem::remove(file);
        continue;
      }

      if (dump->format_version == config.dump_format_version) {
        dumps.push_back(std::move(*dump));
      }
    }

    std::sort(dumps.begin(), dumps.end(),
              [](const ParsedDumpName& a, const ParsedDumpName& b) {
                return a.update_time > b.update_time;
              });

    for (size_t i = config.max_dump_count; i < dumps.size(); ++i) {
      const std::string dump_path = FilenameToPath(dumps[i].filename, config);
      LOG_DEBUG() << "Removing an excessive dump \"" << dump_path
                  << "\" for cache " << cache_name_;
      boost::filesystem::remove(dump_path);
    }
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while cleaning up old dumps for cache " << cache_name_
                << ". Cause: " << ex;
  }
}

std::string Dumper::FilenameToPath(std::string_view filename,
                                   const CacheConfigStatic& config) {
  return fmt::format(FMT_COMPILE("{}/{}"), config.dump_directory, filename);
}

std::string Dumper::GenerateDumpPath(TimePoint update_time,
                                     const CacheConfigStatic& config) {
  return fmt::format(FMT_COMPILE("{}/{}-v{}"), config.dump_directory,
                     utils::datetime::Timestring(update_time, kTimeZone,
                                                 kDumpFilenameDateFormat),
                     config.dump_format_version);
}

std::string Dumper::GenerateFilenameRegex(FileFormatType type) {
  return std::string{
             R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6})-v(\d+))"} +
         (type == FileFormatType::kTmp ? "\\.tmp$" : "$");
}

TimePoint Dumper::MinAcceptableUpdateTime(const CacheConfigStatic& config) {
  return config.max_dump_age
             ? Round(utils::datetime::Now()) - *config.max_dump_age
             : TimePoint::min();
}

TimePoint Dumper::Round(std::chrono::system_clock::time_point time) {
  return std::chrono::round<TimePoint::duration>(time);
}

}  // namespace cache