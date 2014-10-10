// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#ifndef KINGDB_STORAGE_ENGINE_H_
#define KINGDB_STORAGE_ENGINE_H_

#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include "kingdb/kdb.h"
#include "kingdb/options.h"
#include "util/hash.h"
#include "kingdb/common.h"
#include "kingdb/byte_array.h"
#include "util/crc32c.h"
#include "util/file.h"


namespace kdb {

// TODO: split into .cc and .h files
// TODO: split the classes into their own respective files

class FileResourceManager {
 public:
  FileResourceManager() {
  }

  void ResetDataForFileId(uint32_t fileid) {
    num_writes_in_progress_.erase(fileid);
    logindexes_.erase(fileid);
    has_padding_in_values_.erase(fileid);
  }

  uint64_t GetFileSize(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    return filesizes_[fileid];
  }

  void SetFileSize(uint32_t fileid, uint64_t filesize) {
    std::unique_lock<std::mutex> lock(mutex_);
    filesizes_[fileid] = filesize;
  }

  bool IsFileLarge(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    return (largefiles_.find(fileid) != largefiles_.end());
  }

  void SetFileLarge(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    largefiles_.insert(fileid);
  }

  bool IsFileCompacted(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    return (compactedfiles_.find(fileid) != compactedfiles_.end());
  }

  void SetFileCompacted(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    compactedfiles_.insert(fileid);
  }

  uint32_t GetNumWritesInProgress(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    return num_writes_in_progress_[fileid];
  }

  uint32_t SetNumWritesInProgress(uint32_t fileid, int inc) {
    // The number of writers to a specific file is being tracked so that if a
    // file is flushed but is still being written to due to some multi-chunk
    // entry, we don't write the footer yet. That way, if any crash happens,
    // the file will have no footer, which will force a recovery and discover
    // which entries have corrupted data.
    std::unique_lock<std::mutex> lock(mutex_);
    if (num_writes_in_progress_.find(fileid) == num_writes_in_progress_.end()) {
      num_writes_in_progress_[fileid] = 0;
    }
    num_writes_in_progress_[fileid] += inc;
    return num_writes_in_progress_[fileid];
  }

  const std::vector< std::pair<uint64_t, uint32_t> > GetLogIndex(uint32_t fileid) {
    return logindexes_[fileid];
  }

  void AddLogIndex(uint32_t fileid, std::pair<uint64_t, uint32_t> p) {
    logindexes_[fileid].push_back(p);
  }

  bool HasPaddingInValues(uint32_t fileid) {
    std::unique_lock<std::mutex> lock(mutex_);
    return (has_padding_in_values_.find(fileid) != has_padding_in_values_.end());
  }

  void SetHasPaddingInValues(uint32_t fileid, bool flag) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (flag) {
      has_padding_in_values_.insert(fileid);
    } else {
      has_padding_in_values_.erase(fileid);
    }
  }



 private:
  std::mutex mutex_;
  std::map<uint32_t, uint64_t> filesizes_;
  std::set<uint32_t> largefiles_;
  std::set<uint32_t> compactedfiles_;
  std::map<uint32_t, uint64_t> num_writes_in_progress_;
  std::map<uint32_t, std::vector< std::pair<uint64_t, uint32_t> > > logindexes_;
  std::set<uint32_t> has_padding_in_values_;
};


class LogfileManager {
 public:
  LogfileManager() {
    is_closed_ = true;
    is_read_only_ = true;
    has_file_ = false;
    buffer_has_items_ = false;
  }

  LogfileManager(DatabaseOptions& db_options,
                 std::string dbname,
                 std::string prefix,
                 std::string prefix_compaction,
                 std::string dirpath_locks,
                 FileType filetype_default,
                 bool read_only=false)
      : db_options_(db_options),
        filetype_default_(filetype_default),
        is_read_only_(read_only),
        prefix_(prefix),
        prefix_compaction_(prefix_compaction),
        dirpath_locks_(dirpath_locks) {
    LOG_TRACE("LogfileManager::LogfileManager()", "dbname:%s prefix:%s", dbname.c_str(), prefix.c_str());
    dbname_ = dbname;
    sequence_fileid_ = 0;
    sequence_timestamp_ = 0;
    size_block_ = SIZE_LOGFILE_TOTAL;
    has_file_ = false;
    buffer_has_items_ = false;
    hash_ = MakeHash(db_options.hash);
    is_closed_ = false;
    is_locked_sequence_timestamp_ = false;
    if (!is_read_only_) {
      buffer_raw_ = new char[size_block_*2];
      buffer_index_ = new char[size_block_*2];
    }
  }

  ~LogfileManager() {
    Close();
  }

  void Close() {
    std::unique_lock<std::mutex> lock(mutex_close_);
    if (is_read_only_ || is_closed_) return;
    is_closed_ = true;
    FlushCurrentFile();
    CloseCurrentFile();
    delete[] buffer_raw_;
    delete[] buffer_index_;
  }

  std::string GetPrefix() {
    return prefix_;
  }

  std::string GetFilepath(uint32_t fileid) {
    return dbname_ + "/" + prefix_ + LogfileManager::num_to_hex(fileid); // TODO: optimize here
  }

  std::string GetLockFilepath(uint32_t fileid) {
    return dirpath_locks_ + "/" + LogfileManager::num_to_hex(fileid); // TODO: optimize here
  }

  // File id sequence helpers
  void SetSequenceFileId(uint32_t seq) {
    std::unique_lock<std::mutex> lock(mutex_sequence_fileid_);
    sequence_fileid_ = seq;
    LOG_TRACE("LogfileManager::SetSequenceFileId", "seq:%u", seq);
  }

  uint32_t GetSequenceFileId() {
    std::unique_lock<std::mutex> lock(mutex_sequence_fileid_);
    return sequence_fileid_;
  }

  uint32_t IncrementSequenceFileId(uint32_t inc) {
    std::unique_lock<std::mutex> lock(mutex_sequence_fileid_);
    LOG_TRACE("LogfileManager::IncrementSequenceFileId", "sequence_fileid_:%u, inc:%u", sequence_fileid_, inc);
    sequence_fileid_ += inc;
    return sequence_fileid_;
  }


  // Timestamp sequence helpers
  void SetSequenceTimestamp(uint32_t seq) {
    std::unique_lock<std::mutex> lock(mutex_sequence_timestamp_);
    if (!is_locked_sequence_timestamp_) sequence_timestamp_ = seq;
  }

  uint64_t GetSequenceTimestamp() {
    std::unique_lock<std::mutex> lock(mutex_sequence_timestamp_);
    return sequence_timestamp_;
  }

  uint64_t IncrementSequenceTimestamp(uint64_t inc) {
    std::unique_lock<std::mutex> lock(mutex_sequence_timestamp_);
    if (!is_locked_sequence_timestamp_) sequence_timestamp_ += inc;
    return sequence_timestamp_;
  }

  void LockSequenceTimestamp(uint64_t seq) {
    std::unique_lock<std::mutex> lock(mutex_sequence_timestamp_);
    is_locked_sequence_timestamp_ = true;
    sequence_timestamp_ = seq;
  }


  static std::string num_to_hex(uint64_t num) {
    char buffer[20];
    sprintf(buffer, "%08llX", num);
    return std::string(buffer);
  }
  
  static uint32_t hex_to_num(char* hex) {
    uint32_t num;
    sscanf(hex, "%x", &num);
    return num;
  }

  void OpenNewFile() {
    LOG_EMERG("StorageEngine::OpenNewFile()", "Opening file [%s]: %u", filepath_.c_str(), GetSequenceFileId());
    IncrementSequenceFileId(1);
    IncrementSequenceTimestamp(1);
    filepath_ = GetFilepath(GetSequenceFileId());
    LOG_EMERG("StorageEngine::OpenNewFile()", "Opening file [%s]: %u", filepath_.c_str(), GetSequenceFileId());
    if ((fd_ = open(filepath_.c_str(), O_WRONLY|O_CREAT, 0644)) < 0) {
      LOG_EMERG("StorageEngine::OpenNewFile()", "Could not open file [%s]: %s", filepath_.c_str(), strerror(errno));
      exit(-1); // TODO-3: gracefully handle open() errors
    }
    has_file_ = true;
    fileid_ = GetSequenceFileId();
    timestamp_ = GetSequenceTimestamp();

    // Reserving space for header
    offset_start_ = 0;
    offset_end_ = SIZE_LOGFILE_HEADER;

    // Filling in default header
    struct LogFileHeader lfh;
    lfh.filetype  = filetype_default_;
    lfh.timestamp = timestamp_;
    LogFileHeader::EncodeTo(&lfh, buffer_raw_);
  }

  void CloseCurrentFile() {
    if (!has_file_) return;
    LOG_TRACE("LogfileManager::CloseCurrentFile()", "ENTER - fileid_:%d", fileid_);
    FlushLogIndex();
    close(fd_);
    //IncrementSequenceFileId(1);
    //IncrementSequenceTimestamp(1);
    buffer_has_items_ = false;
    has_file_ = false;
  }

  uint32_t FlushCurrentFile(int force_new_file=0, uint64_t padding=0) {
    if (!has_file_) return 0;
    uint32_t fileid_out = fileid_;
    LOG_TRACE("LogfileManager::FlushCurrentFile()", "ENTER - fileid_:%d, has_file_:%d, buffer_has_items_:%d", fileid_, has_file_, buffer_has_items_);
    if (has_file_ && buffer_has_items_) {
      LOG_TRACE("LogfileManager::FlushCurrentFile()", "has_files && buffer_has_items_ - fileid_:%d", fileid_);
      if (write(fd_, buffer_raw_ + offset_start_, offset_end_ - offset_start_) < 0) {
        LOG_TRACE("StorageEngine::FlushCurrentFile()", "Error write(): %s", strerror(errno));
      }
      file_resource_manager.SetFileSize(fileid_, offset_end_);
      offset_start_ = offset_end_;
      buffer_has_items_ = false;
      LOG_TRACE("LogfileManager::FlushCurrentFile()", "items written - offset_end_:%d | size_block_:%d | force_new_file:%d", offset_end_, size_block_, force_new_file);
    }

    if (padding) {
      offset_end_ += padding;
      offset_start_ = offset_end_;
      file_resource_manager.SetFileSize(fileid_, offset_end_);
      ftruncate(fd_, offset_end_);
      lseek(fd_, 0, SEEK_END);
    }

    if (offset_end_ >= size_block_ || (force_new_file && offset_end_ > SIZE_LOGFILE_HEADER)) {
      LOG_TRACE("LogfileManager::FlushCurrentFile()", "file renewed - force_new_file:%d", force_new_file);
      file_resource_manager.SetFileSize(fileid_, offset_end_);
      CloseCurrentFile();
      //OpenNewFile();
    } else {
      //fileid_out = fileid_out - 1;
    }
    LOG_TRACE("LogfileManager::FlushCurrentFile()", "done!");
    return fileid_out;
  }


  Status FlushLogIndex() {
    if (!has_file_) return Status::OK();
    uint64_t num = file_resource_manager.GetNumWritesInProgress(fileid_);
    LOG_TRACE("LogfileManager::FlushLogIndex()", "ENTER - fileid_:%d - num_writes_in_progress:%llu", fileid_, num);
    if (file_resource_manager.GetNumWritesInProgress(fileid_) == 0) {
      uint64_t size_logindex;
      Status s = WriteLogIndex(fd_, file_resource_manager.GetLogIndex(fileid_), &size_logindex, filetype_default_, file_resource_manager.HasPaddingInValues(fileid_), false);
      uint64_t filesize = file_resource_manager.GetFileSize(fileid_);
      file_resource_manager.SetFileSize(fileid_, filesize + size_logindex);
      return s;
    }
    return Status::OK();
  }


  Status WriteLogIndex(int fd,
                       const std::vector< std::pair<uint64_t, uint32_t> >& logindex_current,
                       uint64_t* size_out,
                       FileType filetype,
                       bool has_padding_in_values,
                       bool has_invalid_entries) {
    uint64_t offset = 0;
    struct LogFileFooterIndex lffi;
    for (auto& p: logindex_current) {
      lffi.hashed_key = p.first;
      lffi.offset_entry = p.second;
      uint32_t length = LogFileFooterIndex::EncodeTo(&lffi, buffer_index_ + offset);
      offset += length;
      LOG_TRACE("StorageEngine::WriteLogIndex()", "hashed_key:[%llu] offset:[%08x]", p.first, p.second);
    }

    uint64_t position = lseek(fd, 0, SEEK_END);
    // NOTE: lseek() will not work to retrieve 'position' if the configs allow logfiles
    // to have sizes larger than (2^32)-1 -- lseek64() could be used, but is not standard on all unixes
    struct LogFileFooter footer;
    footer.filetype = filetype;
    footer.offset_indexes = position;
    footer.num_entries = logindex_current.size();
    footer.magic_number = get_magic_number();
    if (has_padding_in_values) footer.SetFlagHasPaddingInValues();
    if (has_invalid_entries) footer.SetFlagHasInvalidEntries();
    uint32_t length = LogFileFooter::EncodeTo(&footer, buffer_index_ + offset);
    offset += length;

    uint32_t crc32 = crc32c::Value(buffer_index_, offset - 4);
    EncodeFixed32(buffer_index_ + offset - 4, crc32);

    if (write(fd, buffer_index_, offset) < 0) {
      LOG_TRACE("StorageEngine::WriteLogIndex()", "Error write(): %s", strerror(errno));
    }
    *size_out = offset;
    LOG_TRACE("StorageEngine::WriteLogIndex()", "offset_indexes:%u, num_entries:[%lu]", position, logindex_current.size());
    return Status::OK();
  }


  uint64_t WriteFirstChunkLargeOrder(Order& order, uint64_t hashed_key) {
    // TODO: what if the large order is self-contained? then need to do all the
    // actions done for the last chunk in WriteChunk() -- maybe make a new
    // method to factorize that code
    uint64_t fileid_largefile = IncrementSequenceFileId(1);
    uint64_t timestamp_largefile = IncrementSequenceTimestamp(1);
    std::string filepath = GetFilepath(fileid_largefile);
    LOG_TRACE("LogfileManager::WriteFirstChunkLargeOrder()", "enter %s", filepath.c_str());
    int fd = 0;
    if ((fd = open(filepath.c_str(), O_WRONLY|O_CREAT, 0644)) < 0) {
      LOG_EMERG("StorageEngine::WriteFirstChunkLargeOrder()", "Could not open file [%s]: %s", filepath.c_str(), strerror(errno));
      exit(-1); // TODO-3: gracefully handle open() errors
    }

    // Write header
    char buffer[SIZE_LOGFILE_HEADER];
    struct LogFileHeader lfh;
    lfh.filetype  = kCompactedLargeType;
    lfh.timestamp = timestamp_largefile;
    LogFileHeader::EncodeTo(&lfh, buffer);
    if(write(fd, buffer, SIZE_LOGFILE_HEADER) < 0) {
      LOG_TRACE("LogfileManager::FlushLargeOrder()", "Error write(): %s", strerror(errno));
    }

    // Write entry metadata
    struct Entry entry;
    entry.SetTypePut();
    entry.SetEntryFull();
    entry.size_key = order.key->size();
    entry.size_value = order.size_value;
    entry.size_value_compressed = order.size_value_compressed;
    entry.hash = hashed_key;
    entry.crc32 = 0;
    entry.SetHasPadding(false);
    uint32_t size_header = Entry::EncodeTo(db_options_, &entry, buffer);
    key_to_headersize[order.tid][order.key->ToString()] = size_header;
    if(write(fd, buffer, size_header) < 0) {
      LOG_TRACE("LogfileManager::FlushLargeOrder()", "Error write(): %s", strerror(errno));
    }

    // Write key and chunk
    // NOTE: Could also put the key and chunk in the buffer and do a single write
    if(write(fd, order.key->data(), order.key->size()) < 0) {
      LOG_TRACE("LogfileManager::FlushLargeOrder()", "Error write(): %s", strerror(errno));
    }
    if(write(fd, order.chunk->data(), order.chunk->size()) < 0) {
      LOG_TRACE("LogfileManager::FlushLargeOrder()", "Error write(): %s", strerror(errno));
    }

    uint64_t filesize = SIZE_LOGFILE_HEADER + size_header + order.key->size() + order.size_value;
    ftruncate(fd, filesize);
    file_resource_manager.SetFileSize(fileid_largefile, filesize);
    close(fd);
    uint64_t fileid_shifted = fileid_largefile;
    fileid_shifted <<= 32;
    LOG_TRACE("LogfileManager::WriteFirstChunkLargeOrder()", "fileid [%d]", fileid_largefile);
    file_resource_manager.SetNumWritesInProgress(fileid_largefile, 1);
    return fileid_shifted | SIZE_LOGFILE_HEADER;
  }


  uint64_t WriteChunk(Order& order, uint64_t hashed_key, uint64_t location, bool is_large_order) {
    uint32_t fileid = (location & 0xFFFFFFFF00000000) >> 32;
    uint32_t offset_file = location & 0x00000000FFFFFFFF;
    std::string filepath = GetFilepath(fileid);
    LOG_TRACE("LogfileManager::WriteChunk()", "key [%s] filepath:[%s] offset_chunk:%llu", order.key->ToString().c_str(), filepath.c_str(), order.offset_chunk);
    int fd = 0;
    if ((fd = open(filepath.c_str(), O_WRONLY, 0644)) < 0) {
      LOG_EMERG("StorageEngine::WriteChunk()", "Could not open file [%s]: %s", filepath.c_str(), strerror(errno));
      exit(-1); // TODO-3: gracefully handle open() errors
    }

    if (key_to_headersize.find(order.tid) == key_to_headersize.end() ||
        key_to_headersize[order.tid].find(order.key->ToString()) == key_to_headersize[order.tid].end()) {
      LOG_TRACE("LogfileManager::WriteChunk()", "Missing in key_to_headersize[]");
    }

    uint32_t size_header = key_to_headersize[order.tid][order.key->ToString()];

    // Write the chunk
    if (pwrite(fd,
               order.chunk->data(),
               order.chunk->size(),
               offset_file + size_header + order.key->size() + order.offset_chunk) < 0) {
      LOG_TRACE("LogfileManager::WriteChunk()", "Error pwrite(): %s", strerror(errno));
    }

    // If this is a last chunk, the header is written again to save the right size of compressed value,
    // and the crc32 is saved too
    if (order.IsLastChunk()) {
      LOG_TRACE("LogfileManager::WriteChunk()", "Write compressed size: [%s] - size:%llu, compressed size:%llu crc32:0x%08llx", order.key->ToString().c_str(), order.size_value, order.size_value_compressed, order.crc32);
      struct Entry entry;
      entry.SetTypePut();
      entry.SetEntryFull();
      entry.size_key = order.key->size();
      entry.size_value = order.size_value;
      entry.size_value_compressed = order.size_value_compressed;
      if (!is_large_order && entry.IsCompressed()) {
        // NOTE: entry.IsCompressed() makes no sense since compression is
        // handled at database level, not at entry level. All usages of
        // IsCompressed() should be replaced by a check on the database options.
        entry.SetHasPadding(true);
        file_resource_manager.SetHasPaddingInValues(fileid_, true);
      }
      entry.hash = hashed_key;

      // Compute the header a first time to get the data serialized
      char buffer[sizeof(struct Entry)*2];
      uint32_t size_header_new = Entry::EncodeTo(db_options_, &entry, buffer);

      // Compute the checksum for the header and combine it with the one for the
      // key and value, then recompute the header to save the checksum
      uint32_t crc32_header = crc32c::Value(buffer + 4, size_header_new - 4);
      entry.crc32 = crc32c::Combine(crc32_header, order.crc32, entry.size_key + entry.size_value_used());
      size_header_new = Entry::EncodeTo(db_options_, &entry, buffer);
      if (size_header_new != size_header) {
        LOG_EMERG("LogfileManager::WriteChunk()", "Error of encoding: the initial header had a size of %u, and it is now %u. The entry is now corrupted.", size_header, size_header_new);
      }

      if (pwrite(fd, buffer, size_header, offset_file) < 0) {
        LOG_TRACE("LogfileManager::WriteChunk()", "Error pwrite(): %s", strerror(errno));
      }
      
      if (is_large_order && entry.IsCompressed()) {
        uint64_t filesize = SIZE_LOGFILE_HEADER + size_header + order.key->size() + order.size_value_compressed;
        file_resource_manager.SetFileSize(fileid, filesize);
        ftruncate(fd, filesize);
      }

      uint32_t num_writes = file_resource_manager.SetNumWritesInProgress(fileid, -1);
      if (fileid != fileid_ && num_writes == 0) {
        lseek(fd, 0, SEEK_END);
        uint64_t size_logindex;
        FileType filetype = is_large_order ? kCompactedLargeType : filetype_default_;
        WriteLogIndex(fd, file_resource_manager.GetLogIndex(fileid), &size_logindex, filetype, file_resource_manager.HasPaddingInValues(fileid), false);
        uint64_t filesize = file_resource_manager.GetFileSize(fileid);
        filesize += size_logindex;
        file_resource_manager.SetFileSize(fileid, filesize);
        if (is_large_order) file_resource_manager.SetFileLarge(fileid);
        file_resource_manager.ResetDataForFileId(fileid);
      }

    }

    close(fd);
    LOG_TRACE("LogfileManager::WriteChunk()", "all good");
    return location;
  }


  uint64_t WriteFirstChunkOrSmallOrder(Order& order, uint64_t hashed_key) {
    uint64_t location_out = 0;
    struct Entry entry;
    if (order.type == OrderType::Put) {
      entry.SetTypePut();
      entry.SetEntryFull();
      entry.size_key = order.key->size();
      entry.size_value = order.size_value;
      entry.size_value_compressed = order.size_value_compressed;
      entry.hash = hashed_key;
      entry.crc32 = order.crc32;
      if (order.IsSelfContained()) {
        entry.SetHasPadding(false);
      } else {
        entry.SetHasPadding(true);
        file_resource_manager.SetHasPaddingInValues(fileid_, true);
        // TODO: check that the has_padding_in_values field in fields is used during compaction
      }
      uint32_t size_header = Entry::EncodeTo(db_options_, &entry, buffer_raw_ + offset_end_);

      if (order.IsSelfContained()) {
        // Compute the checksum for the header and combine it with the one for the
        // key and value, then recompute the header to save the checksum
        uint32_t crc32_header = crc32c::Value(buffer_raw_ + offset_end_ + 4, size_header - 4);
        entry.crc32 = crc32c::Combine(crc32_header, order.crc32, entry.size_key + entry.size_value_used());
        size_header = Entry::EncodeTo(db_options_, &entry, buffer_raw_ + offset_end_);
      }

      memcpy(buffer_raw_ + offset_end_ + size_header, order.key->data(), order.key->size());
      memcpy(buffer_raw_ + offset_end_ + size_header + order.key->size(), order.chunk->data(), order.chunk->size());

      //map_index[order.key] = fileid_ | offset_end_;
      uint64_t fileid_shifted = fileid_;
      fileid_shifted <<= 32;
      location_out = fileid_shifted | offset_end_;
      file_resource_manager.AddLogIndex(fileid_, std::pair<uint64_t, uint32_t>(hashed_key, offset_end_));
      offset_end_ += size_header + order.key->size() + order.chunk->size();

      if (!order.IsSelfContained()) {
        key_to_headersize[order.tid][order.key->ToString()] = size_header;
        LOG_TRACE("StorageEngine::WriteFirstChunkOrSmallOrder()", "BEFORE fileid_ %u", fileid_);
        file_resource_manager.SetNumWritesInProgress(fileid_, 1);
        FlushCurrentFile(0, order.size_value - order.chunk->size());
        // NOTE: A better way to do it would be to copy things into the buffer, and
        // then for the other chunks, either copy in the buffer if the position
        // to write is >= offset_end_, or do a pwrite() if the position is <
        // offset_end_
        // NOTE: might be better to lseek() instead of doing a large write
        // NOTE: No longer necessary to do the lseek() here, as I'm doing it in
        // the FlushCurrentFile()
        //offset_end_ += order.size_value - order.size_chunk;
        //FlushCurrentFile();
        //ftruncate(fd_, offset_end_);
        //lseek(fd_, 0, SEEK_END);
        LOG_TRACE("StorageEngine::WriteFirstChunkOrSmallOrder()", "AFTER fileid_ %u", fileid_);
      }
      LOG_TRACE("StorageEngine::WriteFirstChunkOrSmallOrder()", "Put [%s]", order.key->ToString().c_str());
    } else { // order.type == OrderType::Remove
      LOG_TRACE("StorageEngine::WriteFirstChunkOrSmallOrder()", "Remove [%s]", order.key->ToString().c_str());
      entry.SetTypeRemove();
      entry.SetEntryFull();
      entry.size_key = order.key->size();
      entry.size_value = 0;
      entry.size_value_compressed = 0;
      entry.crc32 = 0;
      uint32_t size_header = Entry::EncodeTo(db_options_, &entry, buffer_raw_ + offset_end_);
      memcpy(buffer_raw_ + offset_end_ + size_header, order.key->data(), order.key->size());

      uint64_t fileid_shifted = fileid_;
      fileid_shifted <<= 32;
      location_out = fileid_shifted | offset_end_;
      file_resource_manager.AddLogIndex(fileid_, std::pair<uint64_t, uint32_t>(hashed_key, offset_end_));
      offset_end_ += size_header + order.key->size();
    }
    return location_out;
  }

  void WriteOrdersAndFlushFile(std::vector<Order>& orders, std::multimap<uint64_t, uint64_t>& map_index_out) {
    for (auto& order: orders) {

      if (!has_file_) OpenNewFile();

      if (offset_end_ > size_block_) {
        LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "About to flush - offset_end_: %llu | size_key: %d | size_value: %d | size_block_: %llu", offset_end_, order.key->size(), order.size_value, size_block_);
        FlushCurrentFile(true, 0);
      }

      uint64_t hashed_key = hash_->HashFunction(order.key->data(), order.key->size());
      // TODO-13: if the item is self-contained (unique chunk), then no need to
      //       have size_value space, size_value_compressed is enough.

      // TODO-12: If the db is embedded, then all order are self contained,
      //       independently of their sizes. Would the compression and CRC32 still
      //       work? Would storing the data (i.e. choosing between the different
      //       storing functions) still work?

      // NOTE: orders can be of various sizes: when using the storage engine as an
      // embedded engine, orders can be of any size, and when plugging the
      // storage engine to a network server, orders can be chucks of data.

      // 1. The order is the first chunk of a very large entry, so we
      //    create a very large file and write the first chunk in there
      uint64_t location = 0;
      bool is_large_order = order.key->size() + order.size_value > size_block_;
      // TODO: is_large_order should become part of 'struct Order'
      if (is_large_order && order.IsFirstChunk()) {
        // TODO-11: shouldn't this be testing size_value_compressed as well? -- yes, only if the order
        // is a full entry by itself (will happen when the kvstore will be embedded and not accessed
        // through the network), otherwise we don't know yet what the total compressed size will be.
        LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "1. key: [%s] size_chunk:%llu offset_chunk: %llu", order.key->ToString().c_str(), order.chunk->size(), order.offset_chunk);
        location = WriteFirstChunkLargeOrder(order, hashed_key);
      // 2. The order is a non-first chunk, so we
      //    open the file, pwrite() the chunk, and close the file.
      } else if (order.offset_chunk != 0) {
        //  TODO-11: replace the tests on compression "order.size_value_compressed ..." by a real test on a flag or a boolean
        //  TODO-11: replace the use of size_value or size_value_compressed by a unique size() which would already return the right value
        LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "2. key: [%s] size_chunk:%llu offset_chunk: %llu", order.key->ToString().c_str(), order.chunk->size(), order.offset_chunk);
        if (key_to_location.find(order.tid) == key_to_location.end()) {
          location = 0;
        } else {
          location = key_to_location[order.tid][order.key->ToString()];
        }
        if (location != 0) {
          WriteChunk(order, hashed_key, location, is_large_order);
        } else {
          LOG_EMERG("StorageEngine", "Avoided catastrophic location error"); 
        }

      // 3. The order is the first chunk of a small or self-contained entry
      } else {
        LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "3. key: [%s] size_chunk:%llu offset_chunk: %llu", order.key->ToString().c_str(), order.chunk->size(), order.offset_chunk);
        buffer_has_items_ = true;
        location = WriteFirstChunkOrSmallOrder(order, hashed_key);
      }

      // If the order was the self-contained or the last chunk, add his location to the output map_index_out[]
      if (order.IsSelfContained() || order.IsLastChunk()) {
        LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "END OF ORDER key: [%s] size_chunk:%llu offset_chunk: %llu location:%llu", order.key->ToString().c_str(), order.chunk->size(), order.offset_chunk, location);
        if (location != 0) {
          map_index_out.insert(std::pair<uint64_t, uint64_t>(hashed_key, location));
        } else {
          LOG_EMERG("StorageEngine", "Avoided catastrophic location error"); 
        }
        if (key_to_location.find(order.tid) != key_to_location.end()) {
          key_to_location[order.tid].erase(order.key->ToString());
        }
        if (key_to_headersize.find(order.tid) != key_to_headersize.end()) {
          key_to_headersize[order.tid].erase(order.key->ToString());
        }
      // Else, if the order is not self-contained and is the first chunk,
      // the location is saved in key_to_location[]
      } else if (order.IsFirstChunk()) {
        if (location != 0 && order.type != OrderType::Remove) {
          key_to_location[order.tid][order.key->ToString()] = location;
        } else {
          LOG_EMERG("StorageEngine", "Avoided catastrophic location error"); 
        }
      }
    }
    LOG_TRACE("StorageEngine::WriteOrdersAndFlushFile()", "end flush");
    FlushCurrentFile(0, 0);
  }


  Status LoadDatabase(std::string& dbname,
                      std::multimap<uint64_t, uint64_t>& index_se,
                      std::set<uint32_t>* fileids_ignore=nullptr,
                      uint32_t fileid_end=0,
                      std::vector<uint32_t>* fileids_iterator=nullptr) {
    Status s;
    struct stat info;

    if (   stat(dbname.c_str(), &info) != 0
        && db_options_.create_if_missing
        && (   mkdir(dbname.c_str(), 0755) < 0
            || mkdir(dirpath_locks_.c_str(), 0755) < 0)) {
      return Status::IOError("Could not create directory", strerror(errno));
    }

    if(!(info.st_mode & S_IFDIR)) {
      return Status::IOError("A file with same name as the database already exists and is not a directory. Remove or rename this file to continue.", dbname.c_str());
    }

    if (!is_read_only_) {
      s = FileUtil::remove_files_with_prefix(dbname_.c_str(), prefix_compaction_);
      if (!s.IsOK()) return Status::IOError("Could not clean up previous compaction");
      s = RemoveAllLockedFiles(dbname_);
      if (!s.IsOK()) return Status::IOError("Could not clean up snapshots");
      s = FileUtil::remove_files_with_prefix(dirpath_locks_.c_str(), "");
      if (!s.IsOK()) return Status::IOError("Could not clean up locks");
    }

    DIR *directory;
    struct dirent *entry;
    if ((directory = opendir(dbname.c_str())) == NULL) {
      return Status::IOError("Could not open database directory", dbname.c_str());
    }

    // Sort the fileids by <timestamp, fileid>, so that puts and removes can be
    // applied in the right order.
    // Indeed, imagine that we have files with ids from 1 to 100, and a
    // compaction process operating on files 1 through 50. The files 1-50 are
    // going to be compacted and the result of this compaction written
    // to ids 101 and above, which means that even though the entries in
    // files 101 and above are older than the entries in files 51-100, they are
    // in files with greater ids. Thus, the file ids cannot be used as a safe
    // way to order the entries in a set of files, and we need to have a sequence id
    // which will allow all other processes to know what is the order of
    // the entries in a set of files, which is why we have a 'timestamp' in each
    // file. As a consequence, the sequence id is the concatenation of
    // the 'timestamp' and the 'fileid'.
    // As the compaction process will always include at least one uncompacted
    // file, the maximum timestamp is garanteed to be always increasing and no
    // overlapping will occur.
    std::map<std::string, uint32_t> timestamp_fileid_to_fileid;
    char filepath[2048];
    char buffer_key[128];
    uint32_t fileid_max = 0;
    uint64_t timestamp_max = 0;
    uint32_t fileid = 0;
    while ((entry = readdir(directory)) != NULL) {
      sprintf(filepath, "%s/%s", dbname.c_str(), entry->d_name);
      if (strncmp(entry->d_name, prefix_compaction_.c_str(), prefix_compaction_.size()) == 0) continue;
      if (stat(filepath, &info) != 0 || !(info.st_mode & S_IFREG)) continue;
      fileid = LogfileManager::hex_to_num(entry->d_name);
      if (   fileids_ignore != nullptr
          && fileids_ignore->find(fileid) != fileids_ignore->end()) {
        LOG_TRACE("LogfileManager::LoadDatabase()", "Skipping file in fileids_ignore:: [%s] [%lld] [%u]\n", entry->d_name, info.st_size, fileid);
        continue;
      }
      if (fileid_end != 0 && fileid > fileid_end) {
        LOG_TRACE("LogfileManager::LoadDatabase()", "Skipping file with id larger than fileid_end (%u): [%s] [%lld] [%u]\n", fileid, entry->d_name, info.st_size, fileid);
        continue;
      }
      LOG_TRACE("LogfileManager::LoadDatabase()", "file: [%s] [%lld] [%u]\n", entry->d_name, info.st_size, fileid);
      if (info.st_size <= SIZE_LOGFILE_HEADER) {
        LOG_TRACE("LogfileManager::LoadDatabase()", "file: [%s] only has a header or less, skipping\n", entry->d_name);
        continue;
      }

      Mmap mmap(filepath, info.st_size);
      struct LogFileHeader lfh;
      Status s = LogFileHeader::DecodeFrom(mmap.datafile(), mmap.filesize(), &lfh);
      if (!s.IsOK()) {
        LOG_TRACE("LogfileManager::LoadDatabase()", "file: [%s] has an invalid header, skipping\n", entry->d_name);
        continue;
      }

      sprintf(buffer_key, "%016llX-%016X", lfh.timestamp, fileid);
      std::string key(buffer_key);
      timestamp_fileid_to_fileid[key] = fileid;
      fileid_max = std::max(fileid_max, fileid);
      timestamp_max = std::max(timestamp_max, lfh.timestamp);
    }

    for (auto& p: timestamp_fileid_to_fileid) {
      uint32_t fileid = p.second;
      if (fileids_iterator != nullptr) fileids_iterator->push_back(fileid);
      std::string filepath = GetFilepath(fileid);
      LOG_TRACE("LogfileManager::LoadDatabase()", "Loading file:[%s] with key:[%s]", filepath.c_str(), p.first.c_str());
      if (stat(filepath.c_str(), &info) != 0) continue;
      Mmap mmap(filepath.c_str(), info.st_size);
      uint64_t filesize;
      bool is_file_large, is_file_compacted;
      s = LoadFile(mmap, fileid, index_se, &filesize, &is_file_large, &is_file_compacted);
      if (s.IsOK()) { 
        file_resource_manager.SetFileSize(fileid, filesize);
        if (is_file_large) file_resource_manager.SetFileLarge(fileid);
        if (is_file_compacted) file_resource_manager.SetFileCompacted(fileid);
      } else if (!s.IsOK() && !is_read_only_) {
        LOG_WARN("LogfileManager::LoadDatabase()", "Could not load index in file [%s], entering recovery mode", filepath.c_str());
        s = RecoverFile(mmap, fileid, index_se);
      }
      if (!s.IsOK() && !is_read_only_) {
        LOG_WARN("LogfileManager::LoadDatabase()", "Recovery failed for file [%s]", filepath.c_str());
        mmap.Close();
        if (std::remove(filepath.c_str()) != 0) {
          LOG_EMERG("LogfileManager::LoadDatabase()", "Could not remove file [%s]", filepath.c_str());
        }
      }
    }
    if (fileid_max > 0) {
      SetSequenceFileId(fileid_max);
      SetSequenceTimestamp(timestamp_max);
    }
    closedir(directory);
    return Status::OK();
  }

  static Status LoadFile(Mmap& mmap,
                  uint32_t fileid,
                  std::multimap<uint64_t, uint64_t>& index_se,
                  uint64_t *filesize_out=nullptr,
                  bool *is_file_large_out=nullptr,
                  bool *is_file_compacted_out=nullptr) {
    LOG_TRACE("LoadFile()", "Loading [%s] of size:%u, sizeof(LogFileFooter):%u", mmap.filepath(), mmap.filesize(), LogFileFooter::GetFixedSize());

    struct LogFileFooter footer;
    Status s = LogFileFooter::DecodeFrom(mmap.datafile() + mmap.filesize() - LogFileFooter::GetFixedSize(), LogFileFooter::GetFixedSize(), &footer);
    if (!s.IsOK() || footer.magic_number != LogfileManager::get_magic_number()) {
      LOG_TRACE("LoadFile()", "Skipping [%s] - magic_number:[%llu/%llu]", mmap.filepath(), footer.magic_number, get_magic_number());
      return Status::IOError("Invalid footer");
    }
    
    uint32_t crc32_computed = crc32c::Value(mmap.datafile() + footer.offset_indexes, mmap.filesize() - footer.offset_indexes - 4);
    if (crc32_computed != footer.crc32) {
      LOG_TRACE("LoadFile()", "Skipping [%s] - Invalid CRC32:[%08x/%08x]", mmap.filepath(), footer.crc32, crc32_computed);
      return Status::IOError("Invalid footer");
    }
    
    LOG_TRACE("LoadFile()", "Footer OK");
    // The file has a clean footer, load all the offsets in the index
    uint64_t offset_index = footer.offset_indexes;
    struct LogFileFooterIndex lffi;
    for (auto i = 0; i < footer.num_entries; i++) {
      uint32_t length_lffi;
      LogFileFooterIndex::DecodeFrom(mmap.datafile() + offset_index, mmap.filesize() - offset_index, &lffi, &length_lffi);
      uint64_t fileid_shifted = fileid;
      fileid_shifted <<= 32;
      index_se.insert(std::pair<uint64_t, uint64_t>(lffi.hashed_key, fileid_shifted | lffi.offset_entry));
      LOG_TRACE("LoadFile()",
                "Add item to index -- hashed_key:[%llu] offset:[%u] -- offset_index:[%llu]",
                lffi.hashed_key, lffi.offset_entry, offset_index);
      offset_index += length_lffi;
    }
    if (filesize_out) *filesize_out = mmap.filesize();
    if (is_file_large_out) *is_file_large_out = footer.IsTypeLarge() ? true : false;
    if (is_file_compacted_out) *is_file_compacted_out = footer.IsTypeCompacted() ? true : false;
    LOG_TRACE("LoadFile()", "Loaded [%s] num_entries:[%llu]", mmap.filepath(), footer.num_entries);

    return Status::OK();
  }

  Status RecoverFile(Mmap& mmap,
                     uint32_t fileid,
                     std::multimap<uint64_t, uint64_t>& index_se) {
    uint32_t offset = SIZE_LOGFILE_HEADER;
    std::vector< std::pair<uint64_t, uint32_t> > logindex_current;
    bool has_padding_in_values = false;
    bool has_invalid_entries   = false;

    struct LogFileHeader lfh;
    Status s = LogFileHeader::DecodeFrom(mmap.datafile(), mmap.filesize(), &lfh);
    // 1. If the file is a large file, just discard it
    if (!s.IsOK() || lfh.IsTypeLarge()) {
      return Status::IOError("Could not recover file");
    }

    // 2. If the file is a logfile, go over all its entries and verify each one of them
    while (true) {
      struct Entry entry;
      uint32_t size_header;
      Status s = Entry::DecodeFrom(db_options_, mmap.datafile() + offset, mmap.filesize() - offset, &entry, &size_header);
      // NOTE: the uses of sizeof(struct Entry) here make not sense, since this
      // size is variable based on the local architecture
      if (   !s.IsOK()
          || offset + sizeof(struct Entry) >= mmap.filesize()
          || entry.size_key == 0
          || offset + sizeof(struct Entry) + entry.size_key > mmap.filesize()
          || offset + sizeof(struct Entry) + entry.size_key + entry.size_value_offset() > mmap.filesize()) {
        // End of file during recovery, thus breaking out of the while-loop
        break;
      }

      crc32_.ResetThreadLocalStorage();
      crc32_.stream(mmap.datafile() + offset + 4, size_header + entry.size_key + entry.size_value_used() - 4);
      bool is_crc32_valid = (entry.crc32 == crc32_.get());
      if (is_crc32_valid) {
        // Valid content, add to index
        logindex_current.push_back(std::pair<uint64_t, uint32_t>(entry.hash, offset));
        uint64_t fileid_shifted = fileid;
        fileid_shifted <<= 32;
        index_se.insert(std::pair<uint64_t, uint64_t>(entry.hash, fileid_shifted | offset));
      } else {
        is_crc32_valid = false;
        has_invalid_entries = true; 
      }

      if (entry.HasPadding()) has_padding_in_values = true;
      offset += size_header + entry.size_key + entry.size_value_offset();
      LOG_TRACE("LogManager::RecoverFile",
                "Scanned hash [%llu], next offset [%llu] - CRC32:%s stored=0x%08x computed=0x%08x",
                entry.hash, offset, is_crc32_valid?"OK":"ERROR", entry.crc32, crc32_.get());
    }

    // 3. Write a new index at the end of the file with whatever entries could be save
    if (offset > SIZE_LOGFILE_HEADER) {
      mmap.Close();
      int fd;
      if ((fd = open(mmap.filepath(), O_WRONLY, 0644)) < 0) {
        LOG_EMERG("LogManager::RecoverFile()", "Could not open file [%s]: %s", mmap.filepath(), strerror(errno));
        return Status::IOError("Could not open file for recovery", mmap.filepath());
      }
      ftruncate(fd, offset);
      lseek(fd, 0, SEEK_END);
      uint64_t size_logindex;
      WriteLogIndex(fd, logindex_current, &size_logindex, lfh.GetFileType(), has_padding_in_values, has_invalid_entries);
      file_resource_manager.SetFileSize(fileid, mmap.filesize() + size_logindex);
      close(fd);
    } else {
      return Status::IOError("Could not recover file");
    }

    return Status::OK();
  }


  Status RemoveAllLockedFiles(std::string& dbname) {
    std::set<uint32_t> fileids;
    DIR *directory;
    struct dirent *entry;
    if ((directory = opendir(dirpath_locks_.c_str())) == NULL) {
      return Status::IOError("Could not open lock directory", dirpath_locks_.c_str());
    }

    char filepath[2048];
    uint32_t fileid = 0;
    struct stat info;
    while ((entry = readdir(directory)) != NULL) {
      if (strncmp(entry->d_name, ".", 1) == 0) continue;
      fileid = LogfileManager::hex_to_num(entry->d_name);
      fileids.insert(fileid);
    }

    closedir(directory);

    for (auto& fileid: fileids) {
      if (std::remove(GetFilepath(fileid).c_str()) != 0) {
        LOG_EMERG("RemoveAllLockedFiles()", "Could not remove data file [%s]", GetFilepath(fileid).c_str());
      }
    }

    return Status::OK();
  }


  uint64_t static get_magic_number() { return 0x4d454f57; }

 private:
  // Options
  DatabaseOptions db_options_;
  Hash *hash_;
  bool is_read_only_;
  bool is_closed_;
  FileType filetype_default_;
  std::mutex mutex_close_;

  uint32_t fileid_;
  uint32_t sequence_fileid_;
  std::mutex mutex_sequence_fileid_;

  uint64_t timestamp_;
  uint64_t sequence_timestamp_;
  std::mutex mutex_sequence_timestamp_;
  bool is_locked_sequence_timestamp_;

  int size_block_;
  bool has_file_;
  int fd_;
  std::string filepath_;
  uint64_t offset_start_;
  uint64_t offset_end_;
  std::string dbname_;
  char *buffer_raw_;
  char *buffer_index_;
  bool buffer_has_items_;
  kdb::CRC32 crc32_;
  std::string prefix_;
  std::string prefix_compaction_;
  std::string dirpath_locks_;

 public:
  FileResourceManager file_resource_manager;

  // key_to_location is made to be dependent on the id of the thread that
  // originated an order, so that if two writers simultaneously write entries
  // with the same key, they will be properly stored into separate locations.
  // NOTE: if a thread crashes or terminates, its data will *not* be cleaned up.
  // NOTE: is it possible for a chunk to arrive when the file is not yet
  // created, and have it's WriteChunk() fail because of that? If so, need to
  // write in buffer_raw_ instead
  std::map< std::thread::id, std::map<std::string, uint64_t> > key_to_location;
  std::map< std::thread::id, std::map<std::string, uint32_t> > key_to_headersize;
};


class StorageEngine {
 public:
  StorageEngine(DatabaseOptions db_options,
                std::string dbname,
                bool read_only=false, // TODO: this should be part of db_options -- sure about that? what options are stored on disk?
                std::set<uint32_t>* fileids_ignore=nullptr,
                uint32_t fileid_end=0)
      : db_options_(db_options),
        is_read_only_(read_only),
        prefix_compaction_("compaction_"),
        dirpath_locks_(dbname + "/locks"),
        logfile_manager_(db_options, dbname, "", prefix_compaction_, dirpath_locks_, kUncompactedLogType, read_only),
        logfile_manager_compaction_(db_options, dbname, prefix_compaction_, prefix_compaction_, dirpath_locks_, kCompactedLogType, read_only) {
    LOG_TRACE("StorageEngine:StorageEngine()", "dbname: %s", dbname.c_str());
    dbname_ = dbname;
    fileids_ignore_ = fileids_ignore;
    num_readers_ = 0;
    is_compaction_in_progress_ = false;
    sequence_snapshot_ = 0;
    stop_requested_ = false;
    is_closed_ = false;
    if (!is_read_only_) {
      thread_index_ = std::thread(&StorageEngine::ProcessingLoopIndex, this);
      thread_data_ = std::thread(&StorageEngine::ProcessingLoopData, this);
      thread_compaction_ = std::thread(&StorageEngine::ProcessingLoopCompaction, this);
    }
    hash_ = MakeHash(db_options.hash);
    if (!is_read_only_) {
      fileids_iterator_ = nullptr;
    } else {
      fileids_iterator_ = new std::vector<uint32_t>();
    }
    Status s = logfile_manager_.LoadDatabase(dbname, index_, fileids_ignore_, fileid_end, fileids_iterator_);
    if (!s.IsOK()) {
      LOG_EMERG("StorageEngine", "Could not load database");
    }
  }

  ~StorageEngine() {}

  void Close() {
    std::unique_lock<std::mutex> lock(mutex_close_);
    if (is_closed_) return;
    is_closed_ = true;

    // Wait for readers to exit
    AcquireWriteLock();
    logfile_manager_.Close();
    Stop();
    ReleaseWriteLock();

    if (!is_read_only_) {
      LOG_TRACE("StorageEngine::Close()", "join start");
      EventManager::update_index.NotifyWait();
      EventManager::flush_buffer.NotifyWait();
      thread_index_.join();
      thread_data_.join();
      thread_compaction_.join();
      ReleaseAllSnapshots();
      LOG_TRACE("StorageEngine::Close()", "join end");
    }

    if (fileids_ignore_ != nullptr) {
      delete fileids_ignore_; 
    }

    if (fileids_iterator_ != nullptr) {
      delete fileids_iterator_; 
    }


    LOG_TRACE("StorageEngine::Close()", "done");
  }

  bool IsStopRequested() { return stop_requested_; }
  void Stop() { stop_requested_ = true; }


  void ProcessingLoopCompaction() {
    // TODO: have the compaction loop actually do the right thing
    std::chrono::milliseconds duration(200);
    std::chrono::milliseconds forever(100000000000000000);
    while(true) {
      struct stat info;
      if (stat("/tmp/do_compaction", &info) == 0) {
        uint32_t seq = logfile_manager_.GetSequenceFileId();
        Compaction(dbname_, 1, seq+1); 
        std::this_thread::sleep_for(forever);
      }
      if (IsStopRequested()) return;
      std::this_thread::sleep_for(duration);
    }
  }

  void ProcessingLoopData() {
    while(true) {
      // Wait for orders to process
      LOG_TRACE("StorageEngine::ProcessingLoopData()", "start");
      std::vector<Order> orders = EventManager::flush_buffer.Wait();
      if (IsStopRequested()) return;
      LOG_TRACE("StorageEngine::ProcessingLoopData()", "got %d orders", orders.size());

      // Process orders, and create update map for the index
      AcquireWriteLock();
      std::multimap<uint64_t, uint64_t> map_index;
      logfile_manager_.WriteOrdersAndFlushFile(orders, map_index);
      ReleaseWriteLock();

      EventManager::flush_buffer.Done();
      EventManager::update_index.StartAndBlockUntilDone(map_index);
    }
  }

  void ProcessingLoopIndex() {
    while(true) {
      LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "start");
      std::multimap<uint64_t, uint64_t> index_updates = EventManager::update_index.Wait();
      if (IsStopRequested()) return;
      LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "got index_updates");
      mutex_index_.lock();

      /*
      for (auto& p: index_updates) {
        if (p.second == 0) {
          LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "remove [%s] num_items_index [%d]", p.first.c_str(), index_.size());
          index_.erase(p.first);
        } else {
          LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "put [%s]", p.first.c_str());
          index_[p.first] = p.second;
        }
      }
      */

      std::multimap<uint64_t, uint64_t> *index;
      mutex_compaction_.lock();
      if (is_compaction_in_progress_) {
        index = &index_compaction_;
      } else {
        index = &index_;
      }
      mutex_compaction_.unlock();

      for (auto& p: index_updates) {
        //uint64_t hashed_key = hash_->HashFunction(p.first.c_str(), p.first.size());
        LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "hash [%llu] location [%llu]", p.first, p.second);
        index->insert(std::pair<uint64_t,uint64_t>(p.first, p.second));
      }

      /*
      for (auto& p: index_) {
        LOG_TRACE("index_", "%s: %llu", p.first.c_str(), p.second);
      }
      */

      mutex_index_.unlock();
      EventManager::update_index.Done();
      LOG_TRACE("StorageEngine::ProcessingLoopIndex()", "done");
      int temp = 1;
      EventManager::clear_buffer.StartAndBlockUntilDone(temp);
    }
  }

  // NOTE: key_out and value_out must be deleted by the caller
  Status Get(ByteArray* key, ByteArray** value_out, uint64_t *location_out=nullptr) {
    mutex_write_.lock();
    mutex_read_.lock();
    num_readers_ += 1;
    mutex_read_.unlock();
    mutex_write_.unlock();

    bool has_compaction_index = false;
    mutex_compaction_.lock();
    has_compaction_index = is_compaction_in_progress_;
    mutex_compaction_.unlock();

    Status s = Status::NotFound("");
    if (has_compaction_index) s = GetWithIndex(index_compaction_, key, value_out, location_out);
    if (!s.IsOK()) s = GetWithIndex(index_, key, value_out, location_out);

    mutex_read_.lock();
    num_readers_ -= 1;
    LOG_TRACE("Get()", "num_readers_: %d", num_readers_);
    mutex_read_.unlock();
    cv_read_.notify_one();

    return s;
  }

  // IMPORTANT: value_out must be deleled by the caller
  Status GetWithIndex(std::multimap<uint64_t, uint64_t>& index,
                      ByteArray* key,
                      ByteArray** value_out,
                      uint64_t *location_out=nullptr) {
    std::unique_lock<std::mutex> lock(mutex_index_);
    // TODO-26: should not be locking here, instead, should store the hashed key
    // and location from the index and release the lock right away -- should not
    // be locking while calling GetEntry()
    
    LOG_TRACE("StorageEngine::GetWithIndex()", "%s", key->ToString().c_str());

    // NOTE: Since C++11, the relative ordering of elements with equivalent keys
    //       in a multimap is preserved.
    uint64_t hashed_key = hash_->HashFunction(key->data(), key->size());
    auto range = index.equal_range(hashed_key);
    auto rbegin = --range.second;
    auto rend  = --range.first;
    for (auto it = rbegin; it != rend; --it) {
      ByteArray *key_temp;
      Status s = GetEntry(it->second, &key_temp, value_out); 
      LOG_TRACE("StorageEngine::GetWithIndex()", "key:[%s] key_temp:[%s] hashed_key:[%llu] hashed_key_temp:[%llu] size_key:[%llu] size_key_temp:[%llu]", key->ToString().c_str(), key_temp->ToString().c_str(), hashed_key, it->first, key->size(), key_temp->size());
      std::string temp(key_temp->data(), key_temp->size());
      LOG_TRACE("StorageEngine::GetWithIndex()", "key_temp:[%s] size[%d]", temp.c_str(), temp.size());
      if (*key_temp == *key) {
        delete key_temp;
        if (s.IsRemoveOrder()) {
          s = Status::NotFound("Unable to find the entry in the storage engine (remove order)");
        }
        if (location_out != nullptr) *location_out = it->second;
        return s;
      }
      delete key_temp;
      delete *value_out;
    }
    LOG_TRACE("StorageEngine::GetWithIndex()", "%s - not found!", key->ToString().c_str());
    return Status::NotFound("Unable to find the entry in the storage engine");
  }

  // IMPORTANT: key_out and value_out must be deleted by the caller
  Status GetEntry(uint64_t location,
                  ByteArray **key_out,
                  ByteArray **value_out) {
    LOG_TRACE("StorageEngine::GetEntry()", "start");
    Status s = Status::OK();
    // TODO: check that the offset falls into the
    // size of the file, just in case a file was truncated but the index
    // still had a pointer to an entry in at an invalid location --
    // alternatively, we could just let the host program crash, to force a restart
    // which would rebuild the index properly

    uint32_t fileid = (location & 0xFFFFFFFF00000000) >> 32;
    uint32_t offset_file = location & 0x00000000FFFFFFFF;
    uint64_t filesize = 0;
    // NOTE: used to be in mutex_write_ and mutex_read_ -- if crashing, put the
    //       mutexes back
    filesize = logfile_manager_.file_resource_manager.GetFileSize(fileid);

    LOG_TRACE("StorageEngine::GetEntry()", "location:%llu fileid:%u offset_file:%u filesize:%llu", location, fileid, offset_file, filesize);
    std::string filepath = logfile_manager_.GetFilepath(fileid); // TODO: optimize here

    auto key_temp = new SharedMmappedByteArray(filepath, filesize);
    auto value_temp = new SharedMmappedByteArray();
    *value_temp = *key_temp;
    // NOTE: verify that value_temp.size() is indeed filesize -- verified and
    // the size was 0: should the size of an mmapped byte array be the size of
    // the file by default?

    struct Entry entry;
    uint32_t size_header;
    s = Entry::DecodeFrom(db_options_, value_temp->datafile() + offset_file, filesize - offset_file, &entry, &size_header);
    if (!s.IsOK()) return s;

    key_temp->SetOffset(offset_file + size_header, entry.size_key);
    value_temp->SetOffset(offset_file + size_header + entry.size_key, entry.size_value);
    value_temp->SetSizeCompressed(entry.size_value_compressed);
    value_temp->SetCRC32(entry.crc32);

    uint32_t crc32_headerkey = crc32c::Value(value_temp->datafile() + offset_file + 4, size_header + entry.size_key - 4);
    value_temp->SetInitialCRC32(crc32_headerkey);

    if (!entry.IsEntryFull()) {
      LOG_EMERG("StorageEngine::GetEntry()", "Entry is not of type FULL, which is not supported");
      return Status::IOError("Entries of type not FULL are not supported");
    }

    if (entry.IsTypeRemove()) {
      s = Status::RemoveOrder();
      delete value_temp;
      value_temp = nullptr;
    }

    LOG_DEBUG("StorageEngine::GetEntry()", "mmap() out - type remove:%d", entry.IsTypeRemove());
    LOG_TRACE("StorageEngine::GetEntry()", "Sizes: key_temp:%llu value_temp:%llu filesize:%llu", key_temp->size(), value_temp->size(), filesize);

    *key_out = key_temp;
    *value_out = value_temp;
    return s;
  }

  bool IsFileLarge(uint32_t fileid) {
    return logfile_manager_.file_resource_manager.IsFileLarge(fileid);
  }

  Status Compaction(std::string dbname,
                    uint32_t fileid_start,
                    uint32_t fileid_end) {
    // TODO: make sure that all sets, maps and multimaps are cleared whenever
    // they are no longer needed
    
    // TODO: when compaction starts, open() a file and lseek() to reserve disk
    //       space -- or write a bunch of files with the "compaction_" prefix
    //       that will be overwritten when the compacted files are written.

    // TODO: add a new flag in files that says "compacted" or "log", and before
    //       starting any compaction process, select only log files, ignore
    //       compacted ones. (large files are 'compacted' by default).

    // TODO-23: replace the change on is_compaction_in_progress_ by a RAII
    //          WARNING: this is not the only part of the code with this issue,
    //          some code digging in all files is required
    mutex_compaction_.lock();
    is_compaction_in_progress_ = true;
    mutex_compaction_.unlock();

    // Before the compaction starts, make sure all compaction-related files are removed
    Status s;
    s = FileUtil::remove_files_with_prefix(dbname.c_str(), prefix_compaction_);
    if (!s.IsOK()) return Status::IOError("Could not clean up previous compaction", dbname.c_str());


    // 1. Get the files needed for compaction
    // TODO: This is a quick hack to get the files for compaction, by going
    //       through all the files. Fix that to be only the latest non-handled
    //       log files
    LOG_TRACE("Compaction()", "Step 1: Get files between fileids %u and %u", fileid_start, fileid_end);
    std::multimap<uint64_t, uint64_t> index_compaction;
    DIR *directory;
    struct dirent *entry;
    if ((directory = opendir(dbname.c_str())) == NULL) {
      return Status::IOError("Could not open database directory", dbname.c_str());
    }
    char filepath[2048];
    uint32_t fileid = 0;
    struct stat info;
    while ((entry = readdir(directory)) != NULL) {
      sprintf(filepath, "%s/%s", dbname.c_str(), entry->d_name);
      fileid = LogfileManager::hex_to_num(entry->d_name);
      if (   logfile_manager_.file_resource_manager.IsFileCompacted(fileid)
          || stat(filepath, &info) != 0
          || !(info.st_mode & S_IFREG) 
          || fileid < fileid_start
          || fileid > fileid_end
          || info.st_size <= SIZE_LOGFILE_HEADER) {
        continue;
      }
      // NOTE: Here the locations are read directly from the secondary storage,
      //       which could be optimized by reading them from the index in memory. 
      //       One way to do that is to have a temporary index to which all
      //       updates are synced during compaction. That way, the main index is
      //       guaranteed to not be changed, thus all sorts of scans and changes
      //       can be done on it. Once compaction is over, the temporary index
      //       can just be poured into the main index.
      Mmap mmap(filepath, info.st_size);
      s = logfile_manager_.LoadFile(mmap, fileid, index_compaction);
      if (!s.IsOK()) {
        LOG_WARN("LogfileManager::Compaction()", "Could not load index in file [%s]", filepath);
        // TODO: handle the case where a file is found to be damaged during compaction
      }
    }
    closedir(directory);


    // 2. Iterating over all unique hashed keys of index_compaction, and determine which
    // locations of the storage engine index 'index_' with similar hashes will need to be compacted.
    LOG_TRACE("Compaction()", "Step 2: Get unique hashed keys");
    std::vector<std::pair<uint64_t, uint64_t>> index_compaction_se;
    for (auto it = index_compaction.begin(); it != index_compaction.end(); it = index_compaction.upper_bound(it->first)) {
      auto range = index_.equal_range(it->first);
      for (auto it_se = range.first; it_se != range.second; ++it_se) {
        index_compaction_se.push_back(*it_se);
      }
    }
    index_compaction.clear(); // no longer needed


    // 3. For each entry, determine which location has to be kept, which has to be deleted,
    // and the overall set of file ids that needs to be compacted
    LOG_TRACE("Compaction()", "Step 3: Determine locations");
    std::set<uint64_t> locations_delete;
    std::set<uint32_t> fileids_compaction;
    std::set<uint32_t> fileids_largefiles_keep;
    std::set<std::string> keys_encountered;
    std::multimap<uint64_t, uint64_t> hashedkeys_to_locations_regular_keep;
    std::multimap<uint64_t, uint64_t> hashedkeys_to_locations_large_keep;
    // Reversing the order of the vector to guarantee that
    // the most recent locations are treated first
    std::reverse(index_compaction_se.begin(), index_compaction_se.end());
    for (auto &p: index_compaction_se) {
      ByteArray *key, *value;
      uint64_t& location = p.second;
      uint32_t fileid = (location & 0xFFFFFFFF00000000) >> 32;
      if (fileid > fileid_end) {
        // Make sure that files added after the compacted
        // log files or during the compaction itself are not used
        continue;
      }
      fileids_compaction.insert(fileid);
      Status s = GetEntry(location, &key, &value);
      std::string str_key = key->ToString();
      delete key;
      delete value;

      // For any given key, only the first occurrence, which is the most recent one,
      // has to be kept. The other ones will be deleted. If the first occurrence
      // is a Remove Order, then all occurrences of that key will be deleted.
      if (keys_encountered.find(str_key) == keys_encountered.end()) {
        keys_encountered.insert(str_key);
        if (IsFileLarge(fileid)) {
          hashedkeys_to_locations_large_keep.insert(p);
          fileids_largefiles_keep.insert(fileid);
        } else if (!s.IsRemoveOrder()) {
          hashedkeys_to_locations_regular_keep.insert(p);
        } else {
          locations_delete.insert(location);
        }
      } else {
        locations_delete.insert(location);
      }
    }
    index_compaction_se.clear(); // no longer needed
    keys_encountered.clear(); // no longer needed


    // 4. Building the clusters of locations, indexed by the smallest location
    // per cluster. All the non-smallest locations are stored as secondary
    // locations. Only regular entries are used: it would not make sense
    // to compact large entries anyway.
    LOG_TRACE("Compaction()", "Step 4: Building clusters");
    std::map<uint64_t, std::vector<uint64_t>> hashedkeys_clusters;
    std::set<uint64_t> locations_secondary;
    for (auto it = hashedkeys_to_locations_regular_keep.begin(); it != hashedkeys_to_locations_regular_keep.end(); it = hashedkeys_to_locations_regular_keep.upper_bound(it->first)) {
      auto range = hashedkeys_to_locations_regular_keep.equal_range(it->first);
      std::vector<uint64_t> locations;
      for (auto it_bucket = range.first; it_bucket != range.second; ++it_bucket) {
        LOG_TRACE("Compaction()", "Building clusters - location:%llu", it->second);
        locations.push_back(it->second);
      }
      std::sort(locations.begin(), locations.end());
      hashedkeys_clusters[locations[0]] = locations;
      for (auto i = 1; i < locations.size(); i++) {
        locations_secondary.insert(locations[i]);
      }
    }
    hashedkeys_to_locations_regular_keep.clear();

    /*
     * The compaction needs the following collections:
     *
     * - fileids_compaction: fileids of all files on which compaction must operate
     *     set<uint32_t>
     *
     * - fileids_largefiles_keep: set of fileids that contain large items that must be kept
     *     set<uint32_t>
     *
     * - hashedkeys_clusters: clusters of locations having same hashed keys,
     *   sorted by ascending order of hashed keys and indexed by the smallest
     *   location.
     *     map<uint64_t, std::vector<uint64_t>>
     *
     * - locations_secondary: locations of all entries to keep
     *     set<uint64_t>
     *
     * - locations_delete: locations of all entries to delete
     *     set<uint64_t>
     *
     */

    // 5. Mmapping all the files involved in the compaction
    LOG_TRACE("Compaction()", "Step 5: Mmap() all the files! ALL THE FILES!");
    std::map<uint32_t, Mmap*> mmaps;
    for (auto it = fileids_compaction.begin(); it != fileids_compaction.end(); ++it) {
      uint32_t fileid = *it;
      if (fileids_largefiles_keep.find(fileid) != fileids_largefiles_keep.end()) continue;
      struct stat info;
      std::string filepath = logfile_manager_.GetFilepath(fileid);
      if (stat(filepath.c_str(), &info) != 0 || !(info.st_mode & S_IFREG)) {
        LOG_EMERG("Compaction()", "Error during compaction with file [%s]", filepath.c_str());
      }
      Mmap *mmap = new Mmap(filepath.c_str(), info.st_size);
      mmaps[fileid] = mmap;
    }


    // 6. Now building a vector of orders, that will be passed to the
    //    logmanager_compaction_ object to persist them on disk
    LOG_TRACE("Compaction()", "Step 6: Build order list");
    std::vector<Order> orders;
    uint64_t timestamp_max = 0;
    for (auto it = fileids_compaction.begin(); it != fileids_compaction.end(); ++it) {
      uint32_t fileid = *it;
      if (IsFileLarge(fileid)) continue;
      Mmap* mmap = mmaps[fileid];

      // Read the header to update the maximimum timestamp
      struct LogFileHeader lfh;
      s = LogFileHeader::DecodeFrom(mmap->datafile(), mmap->filesize(), &lfh);
      if (!s.IsOK()) return Status::IOError("Could not read file header during compaction"); // TODO: skip file instead of returning an error 
      timestamp_max = std::max(timestamp_max, lfh.timestamp);

      // Read the footer to get the offset where entries stop
      struct LogFileFooter footer;
      Status s = LogFileFooter::DecodeFrom(mmap->datafile() + mmap->filesize() - LogFileFooter::GetFixedSize(), LogFileFooter::GetFixedSize(), &footer);
      uint32_t crc32_computed = crc32c::Value(mmap->datafile() + footer.offset_indexes, mmap->filesize() - footer.offset_indexes - 4);
      uint64_t offset_end;
      if (   !s.IsOK()
          || footer.magic_number != LogfileManager::get_magic_number()
          || footer.crc32 != crc32_computed) {
        // TODO: handle error
        offset_end = mmap->filesize();
        LOG_TRACE("Compaction()", "Compaction - invalid footer");
      } else {
        offset_end = footer.offset_indexes;
      }

      // Process entries in the file
      uint32_t offset = SIZE_LOGFILE_HEADER;
      while (offset < offset_end) {
        LOG_TRACE("Compaction()", "order list loop - offset:%u offset_end:%u", offset, offset_end);
        struct Entry entry;
        uint32_t size_header;
        Status s = Entry::DecodeFrom(db_options_, mmap->datafile() + offset, mmap->filesize() - offset, &entry, &size_header);
        // NOTE: The checksum is not verified because during the compaction it
        // doesn't matter whether or not the entry is valid. The user will know
        // that an entry is invalid after doing a Get(), and that his choice to
        // emit a 'delete' command if he wants to delete the entry.
        
        // NOTE: the uses of sizeof(struct Entry) here make not sense, since this
        // size is variable based on the local architecture
        if (   !s.IsOK()
            || offset + sizeof(struct Entry) >= mmap->filesize()
            || entry.size_key == 0
            || offset + sizeof(struct Entry) + entry.size_key > mmap->filesize()
            || offset + sizeof(struct Entry) + entry.size_key + entry.size_value_offset() > mmap->filesize()) {
          LOG_TRACE("Compaction()", "Unexpected end of file - mmap->filesize():%d\n", mmap->filesize());
          entry.print();
          break;
        }

        // TODO-19: make function to get location from fileid and offset, and the
        //          fileid and offset from location
        uint64_t fileid_shifted = fileid;
        fileid_shifted <<= 32;
        uint64_t location = fileid_shifted | offset;

        LOG_TRACE("Compaction()", "order list loop - check if we should keep it - fileid:%u offset:%u", fileid, offset);
        if (   locations_delete.find(location) != locations_delete.end()
            || locations_secondary.find(location) != locations_secondary.end()) {
          offset += size_header + entry.size_key + entry.size_value_offset();
          continue;
        }
 
        std::vector<uint64_t> locations;
        if (hashedkeys_clusters.find(location) == hashedkeys_clusters.end()) {
          LOG_TRACE("Compaction()", "order list loop - does not have cluster");
          locations.push_back(location);
        } else {
          LOG_TRACE("Compaction()", "order list loop - has cluster of %d items", hashedkeys_clusters[location].size());
          locations = hashedkeys_clusters[location];
        }

        //for (auto it_location = locations.begin(); it_location != locations.end(); ++it_location) {
          //uint64_t location = *it_location;
        for (auto& location: locations) {
          uint32_t fileid_location = (location & 0xFFFFFFFF00000000) >> 32;
          uint32_t offset_file = location & 0x00000000FFFFFFFF;
          LOG_TRACE("Compaction()", "order list loop - location fileid:%u offset:%u", fileid_location, offset_file);
          Mmap *mmap_location = mmaps[fileid_location];
          struct Entry entry;
          uint32_t size_header;
          Status s = Entry::DecodeFrom(db_options_, mmap->datafile() + offset, mmap->filesize() - offset, &entry, &size_header);

          LOG_TRACE("Compaction()", "order list loop - create byte arrays");
          ByteArray *key   = new SimpleByteArray(mmap_location->datafile() + offset_file + size_header, entry.size_key);
          ByteArray *chunk = new SimpleByteArray(mmap_location->datafile() + offset_file + size_header + entry.size_key, entry.size_value_used());
          LOG_TRACE("Compaction()", "order list loop - push_back() orders");
          orders.push_back(Order{std::this_thread::get_id(),
                                 OrderType::Put,
                                 key,
                                 chunk,
                                 0,
                                 entry.size_value,
                                 entry.size_value_compressed,
                                 entry.crc32});
        }
        offset += size_header + entry.size_key + entry.size_value_offset();
      }
    }


    // 7. Write compacted orders on secondary storage
    LOG_TRACE("Compaction()", "Step 7: Write compacted files");
    std::multimap<uint64_t, uint64_t> map_index;
    // All the resulting files will have the same timestamp, which is the
    // maximum of all the timestamps in the set of files that have been
    // compacted. This will allow the resulting files to be properly ordered
    // during the next database startup or recovery process.
    logfile_manager_compaction_.LockSequenceTimestamp(timestamp_max);
    logfile_manager_compaction_.WriteOrdersAndFlushFile(orders, map_index);
    logfile_manager_compaction_.CloseCurrentFile();
    orders.clear();
    mmaps.clear();


    // 8. Get fileid range from logfile_manager_
    uint32_t num_files_compacted = logfile_manager_compaction_.GetSequenceFileId();
    uint32_t offset_fileid = logfile_manager_.IncrementSequenceFileId(num_files_compacted) - num_files_compacted;
    LOG_TRACE("Compaction()", "Step 8: num_files_compacted:%u offset_fileid:%u", num_files_compacted, offset_fileid);


    // 9. Rename files
    for (auto fileid = 1; fileid <= num_files_compacted; fileid++) {
      uint32_t fileid_new = fileid + offset_fileid;
      LOG_TRACE("Compaction()", "Renaming [%s] into [%s]", logfile_manager_compaction_.GetFilepath(fileid).c_str(),
                                                           logfile_manager_.GetFilepath(fileid_new).c_str());
      if (std::rename(logfile_manager_compaction_.GetFilepath(fileid).c_str(),
                      logfile_manager_.GetFilepath(fileid_new).c_str()) != 0) {
        LOG_EMERG("Compaction()", "Could not rename file");
        // TODO: crash here
      }
      uint64_t filesize = logfile_manager_compaction_.file_resource_manager.GetFileSize(fileid);
      logfile_manager_.file_resource_manager.SetFileSize(fileid_new, filesize);
      logfile_manager_.file_resource_manager.SetFileCompacted(fileid_new);
    }

    
    // 10. Shift returned locations to match renamed files
    LOG_TRACE("Compaction()", "Step 10: Shifting locations");
    std::multimap<uint64_t, uint64_t> map_index_shifted;
    for (auto &p: map_index) {
      const uint64_t& hashedkey = p.first;
      const uint64_t& location = p.second;
      uint32_t fileid = (location & 0xFFFFFFFF00000000) >> 32;
      uint32_t offset_file = location & 0x00000000FFFFFFFF;

      uint32_t fileid_new = fileid + offset_fileid;
      uint64_t fileid_shifted = fileid_new;
      fileid_shifted <<= 32;
      uint64_t location_new = fileid_shifted | offset_file;
      LOG_TRACE("Compaction()", "Shifting [%llu] into [%llu] (fileid [%u] to [%u])", location, location_new, fileid, fileid_new);

      map_index_shifted.insert(std::pair<uint64_t, uint64_t>(hashedkey, location_new));
    }
    map_index.clear();


    // 11. Add the large entries to be kept to the map that will update the 'index_'
    map_index_shifted.insert(hashedkeys_to_locations_large_keep.begin(), hashedkeys_to_locations_large_keep.end());


    // 12. Update the storage engine index_, by removing the locations that have
    //     been compacted, while making sure that the locations that have been
    //     added as the compaction are not removed
    LOG_TRACE("Compaction()", "Step 12: Update the storage engine index_");
    int num_iterations_per_lock = 10;
    int counter_iterations = 0;
    for (auto it = map_index_shifted.begin(); it != map_index_shifted.end(); it = map_index_shifted.upper_bound(it->first)) {

      if (counter_iterations == 0) {
        AcquireWriteLock();
      }
      counter_iterations += 1;

      // For each hashed key, get the group of locations from the index_: all the locations
      // in that group have already been handled during the compaction, except for the ones
      // that have fileids larger than the max fileid 'fileid_end' -- call these 'locations_after'.
      const uint64_t& hashedkey = it->first;
      auto range_index = index_.equal_range(hashedkey);
      std::vector<uint64_t> locations_after;
      for (auto it_bucket = range_index.first; it_bucket != range_index.second; ++it_bucket) {
        const uint64_t& location = it_bucket->second;
        uint32_t fileid = (location & 0xFFFFFFFF00000000) >> 32;
        if (fileid > fileid_end) {
          // Save all the locations for files with fileid that were not part of
          // the compaction process
          locations_after.push_back(location);
        }
      }

      // Erase the bucket, insert the locations from the compaction process, and
      // then insert the locations from the files that were not part of the
      // compaction process started, 'locations_after'
      index_.erase(hashedkey);
      auto range_compaction = map_index_shifted.equal_range(hashedkey);
      index_.insert(range_compaction.first, range_compaction.second);
      for (auto p = locations_after.begin(); p != locations_after.end(); ++p) {
        index_.insert(std::pair<uint64_t, uint64_t>(hashedkey, *p));
      }

      // Release the lock if needed (throttling)
      if (counter_iterations >= num_iterations_per_lock) {
        ReleaseWriteLock();
        counter_iterations = 0;
      }
    }
    ReleaseWriteLock();


    // 13. Put all the locations inserted after the compaction started
    //     stored in 'index_compaction_' into the main index 'index_'
    LOG_TRACE("Compaction()", "Step 13: Transfer index_compaction_ into index_");
    AcquireWriteLock();
    index_.insert(index_compaction_.begin(), index_compaction_.end()); 
    index_compaction_.clear();
    mutex_compaction_.lock();
    is_compaction_in_progress_ = false;
    mutex_compaction_.unlock();
    ReleaseWriteLock();


    // 14. Remove compacted files
    LOG_TRACE("Compaction()", "Step 14: Remove compacted files");
    mutex_snapshot_.lock();
    if (snapshotids_to_fileids_.size() == 0) {
      // No snapshots are in progress, remove the files on the spot
      for (auto& fileid: fileids_compaction) {
        if (fileids_largefiles_keep.find(fileid) != fileids_largefiles_keep.end()) continue;
        LOG_TRACE("Compaction()", "Removing [%s]", logfile_manager_.GetFilepath(fileid).c_str());
        // TODO: free memory associated with the removed file in the file resource manager
        if (std::remove(logfile_manager_.GetFilepath(fileid).c_str()) != 0) {
          LOG_EMERG("Compaction()", "Could not remove file [%s]", logfile_manager_.GetFilepath(fileid).c_str());
        }
      }
    } else {
      // Snapshots are in progress, therefore mark the files and they will be removed when the snapshots are released
      int num_snapshots = snapshotids_to_fileids_.size();
      for (auto& fileid: fileids_compaction) {
        if (fileids_largefiles_keep.find(fileid) != fileids_largefiles_keep.end()) continue;
        for (auto& p: snapshotids_to_fileids_) {
          snapshotids_to_fileids_[p.first].insert(fileid);
        }
        if (num_references_to_unused_files_.find(fileid) == num_references_to_unused_files_.end()) {
          num_references_to_unused_files_[fileid] = 0;
        }
        num_references_to_unused_files_[fileid] += num_snapshots;

        // Create lock file
        std::string filepath_lock = logfile_manager_.GetLockFilepath(fileid);
        int fd;
        if ((fd = open(filepath_lock.c_str(), O_WRONLY|O_CREAT, 0644)) < 0) {
          LOG_EMERG("StorageEngine::Compaction()", "Could not open file [%s]: %s", filepath_lock.c_str(), strerror(errno));
        }
        close(fd);
      }
    }
    mutex_snapshot_.unlock();

    // TODO-20: update changelogs and fsync() wherever necessary (journal, or whatever name, which has
    //          the sequence of operations that can be used to recover)
 
    return Status::OK();
  }

  // START: Helpers for Snapshots
  // Caller must delete fileids_ignore
  Status GetNewSnapshotData(uint32_t *snapshot_id, std::set<uint32_t> **fileids_ignore) {
    std::unique_lock<std::mutex> lock(mutex_snapshot_);
    *snapshot_id = IncrementSequenceSnapshot(1);
    *fileids_ignore = new std::set<uint32_t>();
    for (auto& p: num_references_to_unused_files_) {
      (*fileids_ignore)->insert(p.first);
    }
    return Status::OK();
  }

  Status ReleaseSnapshot(uint32_t snapshot_id) {
    std::unique_lock<std::mutex> lock(mutex_snapshot_);
    if (snapshotids_to_fileids_.find(snapshot_id) == snapshotids_to_fileids_.end()) {
      return Status::IOError("No snapshot with specified id");
    }

    for (auto& fileid: snapshotids_to_fileids_[snapshot_id]) {
      if(num_references_to_unused_files_[fileid] == 1) {
        LOG_TRACE("ReleaseSnapshot()", "Removing [%s]", logfile_manager_.GetFilepath(fileid).c_str());
        if (std::remove(logfile_manager_.GetFilepath(fileid).c_str()) != 0) {
          LOG_EMERG("ReleaseSnapshot()", "Could not remove file [%s]", logfile_manager_.GetFilepath(fileid).c_str());
        }
        if (std::remove(logfile_manager_.GetLockFilepath(fileid).c_str()) != 0) {
          LOG_EMERG("ReleaseSnapshot()", "Could not lock file [%s]", logfile_manager_.GetLockFilepath(fileid).c_str());
        }
      } else {
        num_references_to_unused_files_[fileid] -= 1;
      }
    }

    snapshotids_to_fileids_.erase(snapshot_id);
    return Status::OK();
  }

  Status ReleaseAllSnapshots() {
    for (auto& p: snapshotids_to_fileids_) {
      ReleaseSnapshot(p.first);
    }
  }

  uint64_t GetSequenceSnapshot() {
    std::unique_lock<std::mutex> lock(mutex_sequence_snapshot_);
    return sequence_snapshot_;
  }

  uint64_t IncrementSequenceSnapshot(uint64_t inc) {
    std::unique_lock<std::mutex> lock(mutex_sequence_snapshot_);
    sequence_snapshot_ += inc;
    return sequence_snapshot_;
  }
  
  std::string GetFilepath(uint32_t fileid) {
    return logfile_manager_.GetFilepath(fileid);
  }

  uint32_t FlushCurrentFileForSnapshot() {
    // TODO: flushing the current file is not enough, I also need to make sure
    //       that all the buffers are flushed
    return logfile_manager_.FlushCurrentFile(1, 0);
  }

  std::vector<uint32_t>* GetFileidsIterator() {
    return fileids_iterator_;
  }
  // END: Helpers for Snapshots

 private:
  void AcquireWriteLock() {
    // Also waits for readers to finish
    // NOTE: should this be made its own templated class?
    mutex_write_.lock();
    while(true) {
      std::unique_lock<std::mutex> lock_read(mutex_read_);
      if (num_readers_ == 0) break;
      cv_read_.wait(lock_read);
    }
  }

  void ReleaseWriteLock() {
    mutex_write_.unlock();
  }

  // Options
  DatabaseOptions db_options_;
  Hash *hash_;
  bool is_read_only_;
  std::set<uint32_t>* fileids_ignore_;
  std::string prefix_compaction_;
  std::string dirpath_locks_;

  // Data
  std::string dbname_;
  LogfileManager logfile_manager_;
  std::map<uint64_t, std::string> data_;
  std::thread thread_data_;
  std::condition_variable cv_read_;
  std::mutex mutex_read_;
  std::mutex mutex_write_;
  int num_readers_;

  // Index
  std::multimap<uint64_t, uint64_t> index_;
  std::multimap<uint64_t, uint64_t> index_compaction_;
  std::thread thread_index_;
  std::mutex mutex_index_;

  // Compaction
  LogfileManager logfile_manager_compaction_;
  std::mutex mutex_compaction_;
  bool is_compaction_in_progress_;
  std::thread thread_compaction_;
  std::mutex mutex_fileds_compacted_;
  std::set<uint32_t> fileids_compacted_;
  std::map<uint32_t, uint32_t> num_references_to_unused_files_;

  // Snapshot
  std::mutex mutex_snapshot_;
  std::map< uint32_t, std::set<uint32_t> > snapshotids_to_fileids_;
  std::mutex mutex_sequence_snapshot_;
  uint32_t sequence_snapshot_;
  std::vector<uint32_t> *fileids_iterator_;

  // Stopping and closing
  bool stop_requested_;
  bool is_closed_;
  std::mutex mutex_close_;
};

};

#endif // KINGDB_STORAGE_ENGINE_H_
