#include "zp_data_partition.h"

#include <fstream>
#include <glog/logging.h>
#include "zp_data_server.h"
#include "zp_binlog_sender_thread.h"
#include "rsync.h"

extern ZPDataServer* zp_data_server;

Partition::Partition(const int partition_id, const std::string &log_path, const std::string &data_path)
  : partition_id_(partition_id),
  readonly_(false),
  role_(Role::kNodeSingle),
  repl_state_(ReplState::kNoConnect),
  bgsave_engine_(NULL),
  purged_index_(0) {
    // Partition related path
    log_path_ = NewPartitionPath(log_path, partition_id_);
    data_path_ = NewPartitionPath(data_path, partition_id_);
    sync_path_ = NewPartitionPath(zp_data_server->db_sync_path(), partition_id_);
    bgsave_path_ = NewPartitionPath(zp_data_server->bgsave_path(), partition_id_);
    
    pthread_rwlock_init(&state_rw_, NULL);
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&partition_rw_, &attr);

    // Create db handle
    nemo::Options option; //TODO option args
    LOG(INFO) << "Loading data for partition:" << partition_id_ << "...";
    db_ = std::shared_ptr<nemo::Nemo>(new nemo::Nemo(data_path_, option));
    assert(db_);
    LOG(INFO) << " Success";

    // Binlog
    logger_ = new Binlog(log_path_, kBinlogSize);
  }

Partition::~Partition() {
  {
    slash::MutexLock l(&slave_mutex_);
    std::vector<SlaveItem>::iterator iter = slaves_.begin();

    while (iter != slaves_.end()) {
      delete static_cast<ZPBinlogSenderThread*>(iter->sender);
      iter =  slaves_.erase(iter);
      LOG(INFO) << "Delete BinlogSender from slaves success";
    }
  }
  delete logger_;
  db_.reset();
  delete bgsave_engine_;
  pthread_rwlock_destroy(&partition_rw_);
  pthread_rwlock_destroy(&state_rw_);
  LOG(INFO) << "Partition " << partition_id_ << " exit!!!";
}

// NOTE: slave_mutex should be held
bool Partition::FindSlave(const Node& node) {
  for (auto iter = slaves_.begin(); iter != slaves_.end(); iter++) {
    if (iter->node == node) {
      return true;
    }
  }
  return false;
}

// NOTE: slave_mutex should be held
void Partition::DeleteSlave(const Node& node) {
  //slash::MutexLock l(&slave_mutex_);
  for (auto iter = slaves_.begin(); iter != slaves_.end(); iter++) {
    if (iter->node == node) {
      delete static_cast<ZPBinlogSenderThread*>(iter->sender);
      slaves_.erase(iter);
      DLOG(INFO) << "Delete slave(" << node.ip << ":" << node.port << ") success";
      break;
    }
  }
}


bool Partition::ShouldWaitDBSync() {
  slash::RWLock l(&state_rw_, false);
  DLOG(INFO) << "ShouldWaitDBSync " 
      << (repl_state_ == ReplState::kWaitDBSync ? "true" : "false")
      << ", repl_state: " << ReplStateMsg[repl_state_] << " role: " << RoleMsg[role_];
  return repl_state_ == ReplState::kWaitDBSync;
}

void Partition::SetWaitDBSync() {
  slash::RWLock l(&state_rw_, true);
  assert(ReplState::kShouldConnect == repl_state_);
  repl_state_ = ReplState::kWaitDBSync;
}

void Partition::WaitDBSyncDone() {
  slash::RWLock l(&state_rw_, true);
  assert(ReplState::kWaitDBSync == repl_state_);
  repl_state_ = ReplState::kShouldConnect;
  DLOG(INFO) << "Partition " << partition_id_ << " WaitDBSyncDone  set repl_state: " << ReplStateMsg[repl_state_];
}

bool Partition::ShouldTrySync() {
  slash::RWLock l(&state_rw_, false);
  DLOG(INFO) << "ShouldTrySync " 
    << (repl_state_ == ReplState::kShouldConnect ? "true" : "false")
    <<  ", repl_state: " << ReplStateMsg[repl_state_];
  return repl_state_ == ReplState::kShouldConnect;
}

void Partition::TrySyncDone() {
  slash::RWLock l(&state_rw_, true);
  assert(ReplState::kShouldConnect == repl_state_);
  repl_state_ = ReplState::kConnected;
  DLOG(INFO) << "Partition " << partition_id_ << "TrySyncDone  set repl_state: " << ReplStateMsg[repl_state_];
}

bool Partition::ChangeDb(const std::string& new_path) {
  nemo::Options option;
  // TODO set options
  //option.write_buffer_size = g_pika_conf->write_buffer_size();
  //option.target_file_size_base = g_pika_conf->target_file_size_base();

  std::string tmp_path(data_path_);
  if (tmp_path.back() == '/') {
    tmp_path.resize(tmp_path.size() - 1);
  }
  tmp_path += "_bak";
  slash::DeleteDirIfExist(tmp_path);
  slash::RWLock l(&partition_rw_, true);
  LOG(INFO) << "Prepare change db from: " << tmp_path;
  db_.reset();
  if (0 != slash::RenameFile(data_path_.c_str(), tmp_path)) {
    LOG(WARNING) << "Failed to rename db path when change db, error: " << strerror(errno);
    return false;
  }
 
  if (0 != slash::RenameFile(new_path.c_str(), data_path_.c_str())) {
    DLOG(INFO) << "Rename (" << new_path.c_str() << ", " << data_path_.c_str();
    LOG(WARNING) << "Failed to rename new db path when change db, error: " << strerror(errno);
    return false;
  }
  db_.reset(new nemo::Nemo(data_path_, option));
  assert(db_);
  slash::DeleteDirIfExist(tmp_path);
  LOG(INFO) << "Change Parition " << partition_id_ << " db success";
  return true;
}


////// BGSave //////

// Prepare engine
bool Partition::InitBgsaveEnv() {
  {
    slash::MutexLock l(&bgsave_protector_);
    // Prepare for bgsave dir
    bgsave_info_.start_time = time(NULL);
    char s_time[32];
    int len = strftime(s_time, sizeof(s_time), "%Y%m%d%H%M%S", localtime(&bgsave_info_.start_time));
    bgsave_info_.s_start_time.assign(s_time, len);
    bgsave_info_.path = bgsave_path_ + bgsave_prefix() + std::string(s_time, 8);
    if (!slash::DeleteDirIfExist(bgsave_info_.path)) {
      LOG(WARNING) << "remove exist bgsave dir failed";
      return false;
    }
    slash::CreatePath(bgsave_info_.path, 0755);
    // Prepare for failed dir
    if (!slash::DeleteDirIfExist(bgsave_info_.path + "_FAILED")) {
      LOG(WARNING) << "remove exist fail bgsave dir failed :";
      return false;
    }
  }
  return true;
}

// Prepare bgsave env
bool Partition::InitBgsaveEngine() {
  delete bgsave_engine_;
  nemo::Status result = nemo::BackupEngine::Open(db_.get(), &bgsave_engine_);
  if (!result.ok()) {
    LOG(WARNING) << "open backup engine failed " << result.ToString();
    return false;
  }

  {
    slash::RWLock l(&partition_rw_, true);
    {
      slash::MutexLock l(&bgsave_protector_);
      logger_->GetProducerStatus(&bgsave_info_.filenum, &bgsave_info_.offset);
    }
    result = bgsave_engine_->SetBackupContent();
    if (!result.ok()){
      LOG(WARNING) << "set backup content failed " << result.ToString();
      return false;
    }
  }
  return true;
}

bool Partition::RunBgsaveEngine(const std::string path) {
  // Backup to tmp dir
  nemo::Status result = bgsave_engine_->CreateNewBackup(path);
  DLOG(INFO) << "Create new backup finished.";
  
  if (!result.ok()) {
    LOG(WARNING) << "backup failed :" << result.ToString();
    return false;
  }
  return true;
}

void Partition::Bgsave() {
  // Only one thread can go through
  {
    slash::MutexLock l(&bgsave_protector_);
    if (bgsave_info_.bgsaving) {
      DLOG(INFO) << "Already bgsaving, cancel it";
      return;
    }
    bgsave_info_.bgsaving = true;
  }
  
  // Prepare for Bgsaving
  if (!InitBgsaveEnv() || !InitBgsaveEngine()) {
    ClearBgsave();
    return;
  }
  LOG(INFO) << " BGsave start";

  zp_data_server->BGSaveTaskSchedule(&DoBgsave, static_cast<void*>(this));
}

void Partition::DoBgsave(void* arg) {
  Partition* p = static_cast<Partition*>(arg);
  BGSaveInfo info = p->bgsave_info();

  // Do bgsave
  bool ok = p->RunBgsaveEngine(info.path);

  DLOG(INFO) << "DoBGSave info file " << zp_data_server->local_ip() << ":" << zp_data_server->local_port() << " filenum=" << info.filenum << " offset=" << info.offset;
  // Some output
  std::ofstream out;
  out.open(info.path + "/info", std::ios::in | std::ios::trunc);
  if (out.is_open()) {
    out << (time(NULL) - info.start_time) << "s\n"
      << zp_data_server->local_ip() << "\n" 
      << zp_data_server->local_port() << "\n"
      << info.filenum << "\n"
      << info.offset << "\n";
    out.close();
  }
  if (!ok) {
    std::string fail_path = info.path + "_FAILED";
    slash::RenameFile(info.path.c_str(), fail_path.c_str());
  }
  p->FinishBgsave();
}

bool Partition::Bgsaveoff() {
  {
    slash::MutexLock l(&bgsave_protector_);
    if (!bgsave_info_.bgsaving) {
      return false;
    }
  }
  if (bgsave_engine_ != NULL) {
    bgsave_engine_->StopBackup();
  }
  return true;
}


bool Partition::TryUpdateMasterOffset() {
  // Check dbsync finished
  std::string info_path = sync_path_ + "/" + kBgsaveInfoFile;
  if (!slash::FileExists(info_path)) {
    return false;
  }

  // Got new binlog offset
  std::ifstream is(info_path);
  if (!is) {
    LOG(WARNING) << "Failed to open info file after db sync";
    return false;
  }
  std::string line, master_ip;
  int lineno = 0;
  int64_t filenum = 0, offset = 0, tmp = 0, master_port = 0;
  while (std::getline(is, line)) {
    lineno++;
    if (lineno == 2) {
      master_ip = line;
    } else if (lineno > 2 && lineno < 6) {
      if (!slash::string2l(line.data(), line.size(), &tmp) || tmp < 0) {
        LOG(WARNING) << "Format of info file after db sync error, line : " << line;
        is.close();
        return false;
      }
      if (lineno == 3) { master_port = tmp; }
      else if (lineno == 4) { filenum = tmp; }
      else { offset = tmp; }

    } else if (lineno > 5) {
      LOG(WARNING) << "Format of info file after db sync error, line : " << line;
      is.close();
      return false;
    }
  }
  is.close();
  LOG(INFO) << "Information from dbsync info. master_ip: " << master_ip
    << ", master_port: " << master_port
    << ", filenum: " << filenum
    << ", offset: " << offset;

  // Sanity check
  {
    slash::RWLock l(&state_rw_, false);
    if (master_ip != master_node_.ip || master_port != master_node_.port) {
      LOG(ERROR) << "Error master ip port: " << master_ip << ":" << master_port;
      return false;
    }
  }

  slash::DeleteFile(info_path);
  if (!ChangeDb(sync_path_)) {
    LOG(ERROR) << "Failed to change db";
    return false;
  }

  // Update master offset
  logger_->SetProducerStatus(filenum, offset);
  return true;
}

Status Partition::AddBinlogSender(const Node &node, uint32_t filenum, uint64_t offset) {
  slash::MutexLock l(&slave_mutex_);
  // Check exist
  if (FindSlave(node)) {
    LOG(WARNING) << "BinlogSender for " << node.ip << ":" << node.port << "already exist, we will remove it first";
    DeleteSlave(node);
    //return Status::InvalidArgument("Binlog sender already exist");
  }

  // New slave
  SlaveItem slave;
  slave.node = node;
  gettimeofday(&slave.create_time, NULL);

  // Sanity check
  uint32_t cur_filenum = 0;
  uint64_t cur_offset = 0;
  logger_->GetProducerStatus(&cur_filenum, &cur_offset);
  if (cur_filenum < filenum || (cur_filenum == filenum && cur_offset < offset)) {
    return Status::InvalidArgument("AddBinlogSender invalid binlog offset");
  }

  // Binlog already be purged
  DLOG(INFO) << " We " << (purged_index_ >= filenum ? "will" : "won't") << " TryDBSync, purged_index_=" << purged_index_ << ", filenum=" << filenum;
  if (purged_index_ > filenum) {
    TryDBSync(node.ip, node.port + kPortShiftRsync, cur_filenum);
    return Status::Incomplete("Bgsaving and DBSync first");
  }

  slash::SequentialFile *readfile;
  std::string confile = NewFileName(logger_->filename, filenum);
  if (!slash::NewSequentialFile(confile, &readfile).ok()) {
    return Status::IOError("AddBinlogSender new sequtial file failed");
  }

  ZPBinlogSenderThread *sender = new ZPBinlogSenderThread(this, slave.node.ip, slave.node.port, readfile, filenum, offset);

  if (sender->trim() == 0) {
    sender->StartThread();
    pthread_t tid = sender->thread_id();
    slave.sender_tid = tid;
    slave.sender = sender;

    LOG(INFO) << "AddBinlogSender for node(" << node.ip << ":" << node.port << ") ok, tid is " << slave.sender_tid;
    // Add sender
    //slash::MutexLock l(&slave_mutex_);
    slaves_.push_back(slave);

    return Status::OK();
  } else {
    delete sender;
    LOG(WARNING) << "AddBinlogSender failed";
    return Status::NotFound("AddBinlogSender bad sender");
  }
}

void Partition::CleanRoleEnv() {
  // Clean binlog sender if needed
  {
    slash::MutexLock l(&slave_mutex_);
    for (auto iter = slaves_.begin(); iter != slaves_.end(); ) {
      delete static_cast<ZPBinlogSenderThread*>(iter->sender);
      iter = slaves_.erase(iter);
      LOG(INFO) << "Delete slave success";
    }
  }
  // Clean binlog receiver
  // TODO
}

// Should hold write lock of state_rw_
void Partition::BecomeSingle() {
  CleanRoleEnv();
  DLOG(INFO) << " Partition " << partition_id_ << " BecomeSingle";
  role_ = Role::kNodeSingle;
  repl_state_ = ReplState::kNoConnect;
  readonly_ = false;
}
void Partition::BecomeMaster() {
  CleanRoleEnv();
  DLOG(INFO) << " Partition " << partition_id_ << " BecomeMaster";
  role_ = Role::kNodeMaster;
  repl_state_ = ReplState::kNoConnect;
  readonly_ = false;
}
void Partition::BecomeSlave() {
  CleanRoleEnv();
  LOG(INFO) << " Partition " << partition_id_ << " BecomeSlave, master is " << master_node_.ip << ":" << master_node_.port;
  role_ = Role::kNodeSlave;
  repl_state_ = ReplState::kShouldConnect;
  readonly_ = true;
}

void Partition::Update(const Node &master, const std::vector<Node> &slaves) {
  assert(slaves.size() == kReplicaNum - 1);

  // Update master slave nodes
  bool change_master = false;
  slash::RWLock l(&state_rw_, true);
  if (master_node_ != master) {
    master_node_ = master;
    change_master = true;
  }
  if (slave_nodes_ != slaves) {
    slave_nodes_.clear();
    for (auto slave : slaves) {
      slave_nodes_.push_back(slave);
    }
  }

  // Update role
  Role new_role = Role::kNodeSingle;
  if (zp_data_server->IsSelf(master)) {
    new_role = Role::kNodeMaster;
  }
  for (auto slave : slaves) {
    if (zp_data_server->IsSelf(slave)) {
      new_role = Role::kNodeSlave;
      break;
    }
  }
  if (role_ != new_role) {
    // Change role
    if (new_role == Role::kNodeMaster) {
      BecomeMaster();
    } else if (new_role == Role::kNodeSlave) {
      BecomeSlave();
    } else {
      BecomeSingle(); 
    }
  } else if (change_master && role_ == Role::kNodeSlave) {
    // Change master
    BecomeSlave();
  }
}

std::string NewPartitionPath(const std::string& name, const uint32_t current) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s/%u", name.c_str(), current);
  return std::string(buf);
}

Partition* NewPartition(const std::string log_path, const std::string data_path, const int partition_id, const Node& master, const std::vector<Node> &slaves) {
  Partition* partition = new Partition(partition_id, log_path, data_path);
  partition->Update(master, slaves);
  return partition;
}

void Partition::DoBinlogCommand(Cmd* cmd, client::CmdRequest &req, client::CmdResponse &res, const std::string &raw_msg) {
  DoCommand(cmd, req, res, raw_msg, true);
}

void Partition::DoCommand(Cmd* cmd, client::CmdRequest &req, client::CmdResponse &res,
    const std::string &raw_msg, bool is_from_binlog) {
  std::string key = cmd->key();

  if (!is_from_binlog && cmd->is_write()) {
    if (readonly_) {
      res.set_code(client::StatusCode::kError);
      res.set_msg("readonly mode");
      DLOG(INFO) << "readonly mode, failed to DoCommand  at Partition: " << partition_id_;
      return;
    }
  }

  // Add read lock for no suspend command
  if (!cmd->is_suspend()) {
    pthread_rwlock_rdlock(&partition_rw_);
  }

  // Add RecordLock for write cmd
  if (cmd->is_write()) {
    mutex_record_.Lock(key);
  }
  
  cmd->Do(&req, &res, this);

  if (cmd->is_write()) {
    if (cmd->result().ok() && req.type() != client::Type::SYNC) {
      // Restore Message
      WriteBinlog(raw_msg);
    }
    mutex_record_.Unlock(key);
  }

  if (!cmd->is_suspend()) {
    pthread_rwlock_unlock(&partition_rw_);
  }
}

inline void Partition::WriteBinlog(const std::string &content) {
  logger_->Lock();
  logger_->Put(content);
  logger_->Unlock();
}

void Partition::TryDBSync(const std::string& ip, int port, int32_t top) {
  DLOG(INFO) << "TryDBSync " << ip << ":" << port << ", top=" << top;

  std::string bg_path;
  uint32_t bg_filenum = 0;
  {
    slash::MutexLock l(&bgsave_protector_);
    bg_path = bgsave_info_.path;
    bg_filenum = bgsave_info_.filenum;
  }

  if (0 != slash::IsDir(bg_path) ||                               //Bgsaving dir exist
      !slash::FileExists(NewFileName(logger_->filename, bg_filenum)) ||  //filenum can be found in binglog
      top - bg_filenum > kDBSyncMaxGap) {      //The file is not too old
    // Need Bgsave first
    Bgsave();
  }
  DBSync(ip, port);
}

void Partition::DBSync(const std::string& ip, int port) {
  // Only one DBSync task for every ip_port

  std::string ip_port = slash::IpPortString(ip, port);
  {
    slash::MutexLock l(&db_sync_protector_);
    if (db_sync_slaves_.find(ip_port) != db_sync_slaves_.end()) {
      return;
    }
    db_sync_slaves_.insert(ip_port);
  }

  // Reuse the bg_thread for Bgsave
  // Since we expect Bgsave and DBSync execute serially
  DBSyncArg *arg = new DBSyncArg(this, ip, port);
  zp_data_server->BGSaveTaskSchedule(&DoDBSync, static_cast<void*>(arg));
}

void Partition::DoDBSync(void* arg) {
  DBSyncArg *psync = static_cast<DBSyncArg*>(arg);
  Partition* partition = psync->p;

  //sleep(3);
  DLOG(INFO) << "DBSync begin sendfile " << psync->ip << ":" << psync->port;
  partition->DBSyncSendFile(psync->ip, psync->port);
  
  delete (DBSyncArg*)arg;
}

void Partition::DBSyncSendFile(const std::string& ip, int port) {
  std::string bg_path;
  {
    slash::MutexLock l(&bgsave_protector_);
    bg_path = bgsave_info_.path;
  }
  // Get all files need to send
  std::vector<std::string> descendant;
  if (!slash::GetDescendant(bg_path, descendant)) {
    LOG(WARNING) << "Get Descendant when try to do db sync failed";
    return;
  }

  DLOG(INFO) << "DBSyncSendFile descendant size= " << descendant.size() << " bg_path=" << bg_path;

  // Iterate to send files
  int ret = 0;
  std::string target_path;
  std::string target_dir = NewPartitionPath(".", partition_id_);
  std::string module = kDBSyncModule;
  std::vector<std::string>::iterator it = descendant.begin();
  slash::RsyncRemote remote(ip, port, module, kDBSyncSpeedLimit * 1024);
  for (; it != descendant.end(); ++it) {
    target_path = (*it).substr(bg_path.size() + 1);
    DLOG(INFO) << "--- descendant: " << target_path;
    if (target_path == kBgsaveInfoFile) {
      continue;
    }
    // We need specify the speed limit for every single file
    ret = slash::RsyncSendFile(*it, target_dir + "/" + target_path, remote);
    if (0 != ret) {
      LOG(WARNING) << "rsync send file failed! From: " << *it
        << ", To: " << target_path
        << ", At: " << ip << ":" << port
        << ", Error: " << ret;
      break;
    }
  }
 
  // Clear target path
  slash::RsyncSendClearTarget(bg_path + "/kv", target_dir + "/kv", remote);
  slash::RsyncSendClearTarget(bg_path + "/hash", target_dir + "/hash", remote);
  slash::RsyncSendClearTarget(bg_path + "/list", target_dir + "/list", remote);
  slash::RsyncSendClearTarget(bg_path + "/set", target_dir + "/set", remote);
  slash::RsyncSendClearTarget(bg_path + "/zset", target_dir + "/zset", remote);

  // Send info file at last
  if (0 == ret) {
    if (0 != (ret = slash::RsyncSendFile(bg_path + "/" + kBgsaveInfoFile, target_dir + "/" + kBgsaveInfoFile, remote))) {
      LOG(WARNING) << "send info file failed";
    }
  }

  // remove slave
  std::string ip_port = slash::IpPortString(ip, port);
  {
    slash::MutexLock l(&db_sync_protector_);
    db_sync_slaves_.erase(ip_port);
  }
  if (0 == ret) {
    LOG(INFO) << "rsync send files success";
  }
}

bool Partition::FlushAll() {
  {
    slash::MutexLock l(&bgsave_protector_);
    if (bgsave_info_.bgsaving) {
      return false;
    }
  }
  std::string dbpath = data_path_;
  if (dbpath[dbpath.length() - 1] == '/') {
    dbpath.erase(dbpath.length() - 1);
  }
  int pos = dbpath.find_last_of('/');
  dbpath = dbpath.substr(0, pos);
  dbpath.append("/deleting");

  LOG(INFO) << "Delete old db...";
  db_.reset();
  if (slash::RenameFile(data_path_, dbpath.c_str()) != 0) {
    LOG(WARNING) << "Failed to rename db path when flushall, error: " << strerror(errno);
    return false;
  }
 
  LOG(INFO) << "Prepare open new db...";
  nemo::Options option;
  db_.reset(new nemo::Nemo(data_path_, option));
  assert(db_);
  LOG(INFO) << "open new db success";
  // TODO PurgeDir(dbpath);
  return true; 
}

bool Partition::PurgeLogs(uint32_t to, bool manual, bool force) {
  usleep(300000);

  // Only one thread can go through
  bool expect = false;
  if (!purging_.compare_exchange_strong(expect, true)) {
    LOG(WARNING) << "purge process already exist";
    return false;
  }
  PurgeArg *arg = new PurgeArg();
  arg->p = this;
  arg->to = to;
  arg->manual = manual;
  arg->force = force;
  zp_data_server->BGPurgeTaskSchedule(&DoPurgeLogs, static_cast<void*>(arg));
  return true;
}

void Partition::DoPurgeLogs(void* arg) {
  PurgeArg *ppurge = static_cast<PurgeArg*>(arg);
  Partition* ps = ppurge->p;

  ps->PurgeFiles(ppurge->to, ppurge->manual, ppurge->force);

  ps->ClearPurge();
  delete (PurgeArg*)arg;
}

bool Partition::PurgeFiles(uint32_t to, bool manual, bool force)
{
  std::map<uint32_t, std::string> binlogs;
  if (!GetBinlogFiles(binlogs)) {
    LOG(WARNING) << "Could not get binlog files!";
    return false;
  }

  int delete_num = 0;
  struct stat file_stat;
  int remain_expire_num = binlogs.size() - kBinlogRemainMaxCount;
  std::map<uint32_t, std::string>::iterator it;
  for (it = binlogs.begin(); it != binlogs.end(); ++it) {
    if ((manual && it->first <= to) ||           // Argument bound
        remain_expire_num > 0 ||                 // Expire num trigger
        (stat(((log_path_ + "/" + it->second)).c_str(), &file_stat) == 0 &&     
         file_stat.st_mtime < time(NULL) - kBinlogRemainMaxDay*24*3600)) // Expire time trigger
    {
      // We check this every time to avoid lock when we do file deletion
      if (!CouldPurge(it->first) && !force) {
        LOG(WARNING) << "Could not purge "<< (it->first) << ", since it is already be used";
        return false;
      }

      // Do delete
      slash::Status s = slash::DeleteFile(log_path_ + "/" + it->second);
      if (s.ok()) {
        ++delete_num;
        --remain_expire_num;
      } else {
        LOG(WARNING) << "Purge log file : " << (it->second) <<  " failed! error:" << s.ToString();
      }
    } else {
      // Break when face the first one not satisfied
      // Since the binlogs is order by the file index
      break;
    }
  }
  if (delete_num) {
    LOG(INFO) << "Success purge "<< delete_num;
  }

  return true;
}

bool Partition::GetBinlogFiles(std::map<uint32_t, std::string>& binlogs) {
  std::vector<std::string> children;
  int ret = slash::GetChildren(log_path_, children);
  if (ret != 0){
    LOG(WARNING) << "Get all files in log path failed! error:" << ret; 
    return false;
  }

  int64_t index = 0;
  std::string sindex;
  std::vector<std::string>::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    if ((*it).compare(0, kBinlogPrefixLen, kBinlogPrefix) != 0) {
      continue;
    }
    sindex = (*it).substr(kBinlogPrefixLen);
    if (slash::string2l(sindex.c_str(), sindex.size(), &index) == 1) {
      binlogs.insert(std::pair<uint32_t, std::string>(static_cast<uint32_t>(index), *it)); 
    }
  }
  return true;
}

bool Partition::CouldPurge(uint32_t index) {
  uint32_t pro_num;
  uint64_t tmp;
  logger_->GetProducerStatus(&pro_num, &tmp);

  //index += 10; //remain some more
  if (index > pro_num) {
    return false;
  }
  slash::MutexLock l(&slave_mutex_);
  std::vector<SlaveItem>::iterator it;
  for (it = slaves_.begin(); it != slaves_.end(); ++it) {
    ZPBinlogSenderThread *zpb = static_cast<ZPBinlogSenderThread*>((*it).sender);
    uint32_t filenum = zpb->filenum();
    if (index > filenum) { 
      return false;
    }
  }
  purged_index_ = index + 1;
  return true;
}

void Partition::AutoPurge() {
  if (!PurgeLogs(0, false, false)) {
    DLOG(WARNING) << "Auto purge failed";
    return;
  }
}

void Partition::Dump() {
  slash::RWLock l(&state_rw_, false);
  DLOG(INFO) << "----------------------------";
  DLOG(INFO) << "  +Partition    " << partition_id_ << ": I am a " << (role_ == Role::kNodeMaster ? "master" : "slave");
  DLOG(INFO) << "     -*Master node " << master_node_;

  for (size_t i = 0; i < slave_nodes_.size(); i++) {
    DLOG(INFO) << "     -* slave  " << i << " " <<  slave_nodes_[i];
  }

  {
    DLOG(INFO) << "----------------------------";
    slash::MutexLock l(&slave_mutex_);
    for (size_t i = 0; i < slaves_.size(); i++) {
      DLOG(INFO) << "     -my_slave  " << i << " " <<  slaves_[i].node;
    }
  }
}

