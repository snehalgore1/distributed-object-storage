#include "consensus/raft_node.h"

#include <algorithm>

#include "common/logging.h"

namespace dos {
namespace consensus {

using clock = std::chrono::steady_clock;

const char* RoleName(Role role) {
  switch (role) {
  case Role::kFollower:
    return "follower";
  case Role::kCandidate:
    return "candidate";
  case Role::kLeader:
    return "leader";
  }
  return "unknown";
}

RaftNode::RaftNode(RaftConfig config, RaftTransport* transport, RaftStorage* storage, ApplyFn apply)
    : config_(std::move(config)), transport_(transport), storage_(storage),
      apply_(std::move(apply)),
      rng_(static_cast<std::mt19937::result_type>(std::random_device{}() ^
                                                  std::hash<std::string>{}(config_.id))) {
  const HardState& hs = storage_->LoadedHardState();
  current_term_ = hs.current_term;
  voted_for_ = hs.voted_for;
  log_ = storage_->LoadedLog();
  last_activity_ = clock::now();
  // No leader heard yet: put contact far in the past so the first election
  // (and post-crash elections) are not blocked by the pre-vote leader lease.
  last_leader_contact_ = clock::now() - std::chrono::hours(24);
  election_timeout_ = RandomElectionTimeout();
}

RaftNode::~RaftNode() { Stop(); }

void RaftNode::Start() {
  running_.store(true);
  election_thread_ = std::thread([this] { ElectionLoop(); });
  for (const std::string& peer : config_.peers) {
    peer_threads_.emplace_back([this, peer] { ReplicationLoop(peer); });
  }
}

void RaftNode::Stop() {
  bool was_running = running_.exchange(false);
  if (!was_running) {
    return;
  }
  cv_.notify_all();
  if (election_thread_.joinable()) {
    election_thread_.join();
  }
  for (std::thread& t : peer_threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  peer_threads_.clear();
}

// --- index helpers (mu_ held) ------------------------------------------------

uint64_t RaftNode::LastLogIndexLocked() const { return log_.empty() ? 0 : log_.back().index; }
uint64_t RaftNode::LastLogTermLocked() const { return log_.empty() ? 0 : log_.back().term; }

uint64_t RaftNode::TermAtLocked(uint64_t index) const {
  if (index == 0 || index > log_.size()) {
    return 0;
  }
  return log_[index - 1].term; // log_[k-1].index == k
}

int RaftNode::MajorityLocked() const {
  return static_cast<int>(config_.peers.size() + 1) / 2 + 1;
}

std::chrono::milliseconds RaftNode::RandomElectionTimeout() {
  // chrono::milliseconds::rep is `long long` on libc++ but `long` on libstdc++,
  // so pin std::max's type explicitly to stay portable across the two.
  const long long lo = config_.election_min.count();
  const long long hi = std::max<long long>(lo + 1, config_.election_max.count());
  std::uniform_int_distribution<long long> dist(lo, hi - 1);
  return std::chrono::milliseconds(dist(rng_));
}

void RaftNode::PersistHardStateLocked() {
  storage_->SaveHardState(HardState{current_term_, voted_for_});
}

void RaftNode::StepDownIfStaleLocked(uint64_t term) {
  if (term > current_term_) {
    const bool was_active = role_ != Role::kFollower;
    current_term_ = term;
    voted_for_.clear();
    role_ = Role::kFollower;
    PersistHardStateLocked();
    if (was_active) {
      LogInfo("raft_step_down",
              {{"node", config_.id}, {"term", std::to_string(current_term_)}});
    }
  }
}

// --- RPC handlers ------------------------------------------------------------

void RaftNode::HandleRequestVote(const rpc::RequestVoteRequest& req,
                                 rpc::RequestVoteResponse* resp) {
  std::lock_guard<std::mutex> lock(mu_);

  // Pre-vote: a trial round. It must NOT change our term/vote or reset the
  // election timer. Grant only if we are not the leader, the candidate's
  // would-be term is not stale, its log is at least as up-to-date, and we have
  // not heard from a leader within the election window (the lease that stops a
  // healthy leader from being disrupted).
  if (req.pre_vote()) {
    resp->set_term(current_term_);
    bool grant = false;
    if (req.term() >= current_term_ && role_ != Role::kLeader) {
      const bool up_to_date = req.last_log_term() > LastLogTermLocked() ||
                              (req.last_log_term() == LastLogTermLocked() &&
                               req.last_log_index() >= LastLogIndexLocked());
      const bool leaderless =
          (clock::now() - last_leader_contact_) >= config_.election_min;
      grant = up_to_date && leaderless;
    }
    resp->set_vote_granted(grant);
    return;
  }

  StepDownIfStaleLocked(req.term());

  bool grant = false;
  if (req.term() == current_term_ &&
      (voted_for_.empty() || voted_for_ == req.candidate_id())) {
    // Grant only if the candidate's log is at least as up-to-date (paper §5.4.1).
    const uint64_t my_last_term = LastLogTermLocked();
    const uint64_t my_last_index = LastLogIndexLocked();
    const bool up_to_date = req.last_log_term() > my_last_term ||
                            (req.last_log_term() == my_last_term &&
                             req.last_log_index() >= my_last_index);
    if (up_to_date) {
      grant = true;
      voted_for_ = req.candidate_id();
      PersistHardStateLocked();
      last_activity_ = clock::now();
      election_timeout_ = RandomElectionTimeout();
    }
  }
  resp->set_term(current_term_);
  resp->set_vote_granted(grant);
}

void RaftNode::HandleAppendEntries(const rpc::AppendEntriesRequest& req,
                                   rpc::AppendEntriesResponse* resp) {
  std::lock_guard<std::mutex> lock(mu_);
  resp->set_success(false);
  resp->set_conflict_index(0);

  if (req.term() < current_term_) {
    resp->set_term(current_term_); // reject a stale leader
    return;
  }
  StepDownIfStaleLocked(req.term());

  // Valid leader for the current term: reset the election timer, record leader
  // contact (feeds the pre-vote lease), and note the term.
  role_ = Role::kFollower;
  leader_id_ = req.leader_id();
  last_activity_ = clock::now();
  last_leader_contact_ = last_activity_;
  election_timeout_ = RandomElectionTimeout();
  resp->set_term(current_term_);

  // Log-consistency check at prev_log_index (paper §5.3).
  const uint64_t prev_index = req.prev_log_index();
  if (prev_index > LastLogIndexLocked()) {
    resp->set_conflict_index(LastLogIndexLocked() + 1); // we're missing entries
    return;
  }
  if (prev_index > 0 && TermAtLocked(prev_index) != req.prev_log_term()) {
    resp->set_conflict_index(prev_index); // term mismatch: back the leader up
    return;
  }

  // Append / overwrite. An existing entry that conflicts (same index, different
  // term) and everything after it is dropped, then the new suffix is appended.
  bool changed = false;
  for (const rpc::LogEntry& e : req.entries()) {
    const uint64_t idx = e.index();
    if (idx <= LastLogIndexLocked()) {
      if (TermAtLocked(idx) == e.term()) {
        continue; // already have a matching entry
      }
      log_.resize(idx - 1); // drop the conflicting suffix
    }
    log_.push_back(LogEntry{e.term(), e.index(), e.command()});
    changed = true;
  }
  if (changed) {
    storage_->RewriteLog(log_); // simple and always correct at metadata scale
  }

  resp->set_success(true);
  if (req.leader_commit() > commit_index_) {
    commit_index_ = std::min(req.leader_commit(), LastLogIndexLocked());
    ApplyCommittedLocked();
  }
}

// --- proposal / apply --------------------------------------------------------

StatusOr<uint64_t> RaftNode::Propose(const std::string& command) {
  std::lock_guard<std::mutex> lock(mu_);
  if (role_ != Role::kLeader) {
    return Status::Unavailable("not leader (leader=" + leader_id_ + ")");
  }
  LogEntry entry{current_term_, LastLogIndexLocked() + 1, command};
  log_.push_back(entry);
  storage_->AppendEntry(entry); // leader durability before the entry counts
  AdvanceCommitLocked();        // commits immediately when self is a majority
  cv_.notify_all();             // wake replication loops
  return entry.index;
}

Status RaftNode::WaitApplied(uint64_t index, uint64_t term, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  const auto deadline = clock::now() + timeout;
  while (last_applied_ < index) {
    if (current_term_ > term) {
      return Status::Unavailable("term advanced; proposal may be lost");
    }
    if (index <= LastLogIndexLocked() && TermAtLocked(index) != term) {
      return Status::Unavailable("log entry overwritten before commit");
    }
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      return Status::Unavailable("timed out waiting for commit");
    }
  }
  if (TermAtLocked(index) != term) {
    return Status::Unavailable("log entry overwritten before apply");
  }
  return Status::Ok();
}

void RaftNode::ApplyCommittedLocked() {
  while (last_applied_ < commit_index_) {
    ++last_applied_;
    apply_(log_[last_applied_ - 1]);
  }
  cv_.notify_all(); // wake WaitApplied waiters
}

void RaftNode::AdvanceCommitLocked() {
  // Find the highest index replicated on a majority whose entry is from the
  // current term (paper §5.4.2), then commit up to it.
  const uint64_t last = LastLogIndexLocked();
  for (uint64_t n = last; n > commit_index_; --n) {
    if (TermAtLocked(n) != current_term_) {
      continue;
    }
    int count = 1; // this leader
    for (const std::string& peer : config_.peers) {
      auto it = match_index_.find(peer);
      if (it != match_index_.end() && it->second >= n) {
        ++count;
      }
    }
    if (count >= MajorityLocked()) {
      commit_index_ = n;
      ApplyCommittedLocked();
      break;
    }
  }
}

// --- background loops --------------------------------------------------------

void RaftNode::ElectionLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (running_.load()) {
    if (role_ == Role::kLeader) {
      cv_.wait(lock, [this] { return !running_.load() || role_ != Role::kLeader; });
      continue;
    }
    const auto now = clock::now();
    const auto deadline = last_activity_ + election_timeout_;
    if (now >= deadline) {
      lock.unlock();
      StartElection();
      lock.lock();
    } else {
      cv_.wait_until(lock, deadline);
    }
  }
}

void RaftNode::StartElection() {
  // --- Phase 1: pre-vote at term+1, WITHOUT changing our term or vote. If we
  // cannot win a trial round we stay a follower and disturb no one. ---
  rpc::RequestVoteRequest pre;
  uint64_t would_be_term = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ == Role::kLeader) {
      return;
    }
    would_be_term = current_term_ + 1;
    pre.set_pre_vote(true);
    pre.set_term(would_be_term);
    pre.set_candidate_id(config_.id);
    pre.set_last_log_index(LastLogIndexLocked());
    pre.set_last_log_term(LastLogTermLocked());
  }
  if (!WinVoteRound(pre, /*pre_vote=*/true, would_be_term)) {
    return; // not enough pre-votes (e.g., a healthy leader still leads)
  }

  // --- Phase 2: real election. Only now do we increment the term. ---
  rpc::RequestVoteRequest req;
  uint64_t term = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Abort if the pre-vote round changed our state: a higher term was observed
    // and adopted, we became leader, or a leader contacted us in the meantime.
    if (role_ == Role::kLeader || current_term_ + 1 != would_be_term ||
        (clock::now() - last_leader_contact_) < config_.election_min) {
      return;
    }
    ++current_term_;
    role_ = Role::kCandidate;
    voted_for_ = config_.id;
    leader_id_.clear();
    PersistHardStateLocked();
    last_activity_ = clock::now();
    election_timeout_ = RandomElectionTimeout();
    term = current_term_;
    req.set_pre_vote(false);
    req.set_term(term);
    req.set_candidate_id(config_.id);
    req.set_last_log_index(LastLogIndexLocked());
    req.set_last_log_term(LastLogTermLocked());
    LogInfo("raft_election_started", {{"node", config_.id}, {"term", std::to_string(term)}});
  }

  if (WinVoteRound(req, /*pre_vote=*/false, term)) {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ == Role::kCandidate && current_term_ == term) {
      BecomeLeaderLocked();
    }
  }
}

bool RaftNode::WinVoteRound(const rpc::RequestVoteRequest& req, bool pre_vote, uint64_t term) {
  int votes = 1; // this node
  std::vector<std::thread> requests;
  for (const std::string& peer : config_.peers) {
    requests.emplace_back([this, peer, &req, pre_vote, term, &votes] {
      rpc::RequestVoteResponse resp;
      if (!transport_->SendRequestVote(peer, req, &resp)) {
        return;
      }
      std::lock_guard<std::mutex> lock(mu_);
      if (resp.term() > current_term_) {
        StepDownIfStaleLocked(resp.term()); // we are behind
        cv_.notify_all();
        return;
      }
      // A real vote counts only while we remain the candidate of `term`; a
      // pre-vote round has no such state to guard.
      if (!pre_vote && (role_ != Role::kCandidate || current_term_ != term)) {
        return;
      }
      if (resp.vote_granted()) {
        ++votes; // guarded by mu_
      }
    });
  }
  for (std::thread& t : requests) {
    t.join();
  }
  std::lock_guard<std::mutex> lock(mu_);
  return votes >= MajorityLocked();
}

void RaftNode::BecomeLeaderLocked() {
  role_ = Role::kLeader;
  leader_id_ = config_.id;
  const uint64_t next = LastLogIndexLocked() + 1;
  for (const std::string& peer : config_.peers) {
    next_index_[peer] = next;
    match_index_[peer] = 0;
  }
  LogInfo("raft_leader_elected",
          {{"node", config_.id}, {"term", std::to_string(current_term_)}});
  cv_.notify_all(); // kick replication loops into sending immediate heartbeats
}

void RaftNode::ReplicationLoop(const std::string& peer) {
  std::unique_lock<std::mutex> lock(mu_);
  while (running_.load()) {
    if (role_ != Role::kLeader) {
      cv_.wait(lock, [this] { return !running_.load() || role_ == Role::kLeader; });
      continue;
    }
    const uint64_t term = current_term_;
    lock.unlock();
    ReplicateTo(peer, term);
    lock.lock();
    // Heartbeat cadence; a Propose (notify_all) wakes us early to replicate.
    cv_.wait_for(lock, config_.heartbeat);
  }
}

void RaftNode::ReplicateTo(const std::string& peer, uint64_t term) {
  rpc::AppendEntriesRequest req;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::kLeader || current_term_ != term) {
      return;
    }
    const uint64_t next = next_index_.count(peer) ? next_index_[peer] : LastLogIndexLocked() + 1;
    const uint64_t prev = next - 1;
    req.set_term(term);
    req.set_leader_id(config_.id);
    req.set_prev_log_index(prev);
    req.set_prev_log_term(TermAtLocked(prev));
    req.set_leader_commit(commit_index_);
    for (uint64_t i = next; i <= LastLogIndexLocked(); ++i) {
      const LogEntry& e = log_[i - 1];
      rpc::LogEntry* pe = req.add_entries();
      pe->set_term(e.term);
      pe->set_index(e.index);
      pe->set_command(e.command);
    }
  }

  rpc::AppendEntriesResponse resp;
  if (!transport_->SendAppendEntries(peer, req, &resp)) {
    return; // peer unreachable; try again next tick
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (current_term_ != term || role_ != Role::kLeader) {
    return;
  }
  if (resp.term() > current_term_) {
    StepDownIfStaleLocked(resp.term());
    cv_.notify_all();
    return;
  }
  if (resp.success()) {
    const uint64_t matched = req.prev_log_index() + static_cast<uint64_t>(req.entries_size());
    match_index_[peer] = std::max(match_index_[peer], matched);
    next_index_[peer] = match_index_[peer] + 1;
    AdvanceCommitLocked();
  } else {
    uint64_t conflict = resp.conflict_index();
    next_index_[peer] = conflict == 0 ? 1 : conflict; // back up and retry
  }
}

// --- observability -----------------------------------------------------------

Role RaftNode::role() const {
  std::lock_guard<std::mutex> lock(mu_);
  return role_;
}
uint64_t RaftNode::current_term() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_term_;
}
std::string RaftNode::leader_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leader_id_;
}
uint64_t RaftNode::commit_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return commit_index_;
}
uint64_t RaftNode::last_log_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return LastLogIndexLocked();
}

} // namespace consensus
} // namespace dos
