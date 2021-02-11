#pragma once

#include "dolog.hh"

class TCPClientThreadData
{
public:
  TCPClientThreadData(): localRespRulactions(g_resprulactions.getLocal()), mplexer(std::unique_ptr<FDMultiplexer>(FDMultiplexer::getMultiplexerSilent()))
  {
  }

  LocalHolders holders;
  LocalStateHolder<vector<DNSDistResponseRuleAction> > localRespRulactions;
  std::unique_ptr<FDMultiplexer> mplexer{nullptr};
};

struct ConnectionInfo
{
  ConnectionInfo(ClientState* cs_): cs(cs_), fd(-1)
  {
  }
  ConnectionInfo(ConnectionInfo&& rhs): remote(rhs.remote), cs(rhs.cs), fd(rhs.fd)
  {
    rhs.cs = nullptr;
    rhs.fd = -1;
  }

  ConnectionInfo(const ConnectionInfo& rhs) = delete;
  ConnectionInfo& operator=(const ConnectionInfo& rhs) = delete;

  ConnectionInfo& operator=(ConnectionInfo&& rhs)
  {
    remote = rhs.remote;
    cs = rhs.cs;
    rhs.cs = nullptr;
    fd = rhs.fd;
    rhs.fd = -1;
    return *this;
  }

  ~ConnectionInfo()
  {
    if (fd != -1) {
      close(fd);
      fd = -1;
    }

    if (cs) {
      --cs->tcpCurrentConnections;
    }
  }

  ComboAddress remote;
  ClientState* cs{nullptr};
  int fd{-1};
};

class IncomingTCPConnectionState
{
public:
  IncomingTCPConnectionState(ConnectionInfo&& ci, TCPClientThreadData& threadData, const struct timeval& now): d_buffer(s_maxPacketCacheEntrySize), d_threadData(threadData), d_ci(std::move(ci)), d_handler(d_ci.fd, g_tcpRecvTimeout, d_ci.cs->tlsFrontend ? d_ci.cs->tlsFrontend->getContext() : nullptr, now.tv_sec), d_ioState(make_unique<IOStateHandler>(threadData.mplexer, d_ci.fd)), d_connectionStartTime(now)
  {
    d_origDest.reset();
    d_origDest.sin4.sin_family = d_ci.remote.sin4.sin_family;
    socklen_t socklen = d_origDest.getSocklen();
    if (getsockname(d_ci.fd, reinterpret_cast<sockaddr*>(&d_origDest), &socklen)) {
      d_origDest = d_ci.cs->local;
    }
    /* belongs to the handler now */
    d_ci.fd = -1;
    d_proxiedDestination = d_origDest;
    d_proxiedRemote = d_ci.remote;
  }

  IncomingTCPConnectionState(const IncomingTCPConnectionState& rhs) = delete;
  IncomingTCPConnectionState& operator=(const IncomingTCPConnectionState& rhs) = delete;

  ~IncomingTCPConnectionState();

  void resetForNewQuery();

  boost::optional<struct timeval> getClientReadTTD(struct timeval now) const
  {
    if (g_maxTCPConnectionDuration == 0 && g_tcpRecvTimeout == 0) {
      return boost::none;
    }

    if (g_maxTCPConnectionDuration > 0) {
      auto elapsed = now.tv_sec - d_connectionStartTime.tv_sec;
      if (elapsed < 0 || (static_cast<size_t>(elapsed) >= g_maxTCPConnectionDuration)) {
        return now;
      }
      auto remaining = g_maxTCPConnectionDuration - elapsed;
      if (g_tcpRecvTimeout == 0 || remaining <= static_cast<size_t>(g_tcpRecvTimeout)) {
        now.tv_sec += remaining;
        return now;
      }
    }

    now.tv_sec += g_tcpRecvTimeout;
    return now;
  }

  boost::optional<struct timeval> getClientWriteTTD(const struct timeval& now) const
  {
    if (g_maxTCPConnectionDuration == 0 && g_tcpSendTimeout == 0) {
      return boost::none;
    }

    struct timeval res = now;

    if (g_maxTCPConnectionDuration > 0) {
      auto elapsed = res.tv_sec - d_connectionStartTime.tv_sec;
      if (elapsed < 0 || static_cast<size_t>(elapsed) >= g_maxTCPConnectionDuration) {
        return res;
      }
      auto remaining = g_maxTCPConnectionDuration - elapsed;
      if (g_tcpSendTimeout == 0 || remaining <= static_cast<size_t>(g_tcpSendTimeout)) {
        res.tv_sec += remaining;
        return res;
      }
    }

    res.tv_sec += g_tcpSendTimeout;
    return res;
  }

  bool maxConnectionDurationReached(unsigned int maxConnectionDuration, const struct timeval& now)
  {
    if (maxConnectionDuration) {
      time_t curtime = now.tv_sec;
      unsigned int elapsed = 0;
      if (curtime > d_connectionStartTime.tv_sec) { // To prevent issues when time goes backward
        elapsed = curtime - d_connectionStartTime.tv_sec;
      }
      if (elapsed >= maxConnectionDuration) {
        return true;
      }
      d_remainingTime = maxConnectionDuration - elapsed;
    }

    return false;
  }

  std::shared_ptr<TCPConnectionToBackend> getActiveDownstreamConnection(const std::shared_ptr<DownstreamState>& ds, const std::unique_ptr<std::vector<ProxyProtocolValue>>& tlvs);
  std::shared_ptr<TCPConnectionToBackend> getDownstreamConnection(std::shared_ptr<DownstreamState>& ds, const std::unique_ptr<std::vector<ProxyProtocolValue>>& tlvs, const struct timeval& now);
  void registerActiveDownstreamConnection(std::shared_ptr<TCPConnectionToBackend>& conn);

  std::unique_ptr<FDMultiplexer>& getIOMPlexer() const
  {
    return d_threadData.mplexer;
  }

  static void handleIO(std::shared_ptr<IncomingTCPConnectionState>& conn, const struct timeval& now);
  static void handleIOCallback(int fd, FDMultiplexer::funcparam_t& param);
  static void notifyIOError(std::shared_ptr<IncomingTCPConnectionState>& state, IDState&& query, const struct timeval& now);
  static IOState sendResponse(std::shared_ptr<IncomingTCPConnectionState>& state, const struct timeval& now, TCPResponse&& response);
  static void queueResponse(std::shared_ptr<IncomingTCPConnectionState>& state, const struct timeval& now, TCPResponse&& response);

  /* we take a copy of a shared pointer, not a reference, because the initial shared pointer might be released during the handling of the response */
  static void handleResponse(std::shared_ptr<IncomingTCPConnectionState> state, const struct timeval& now, TCPResponse&& response);
  static void handleXFRResponse(std::shared_ptr<IncomingTCPConnectionState>& state, const struct timeval& now, TCPResponse&& response);
  static void handleTimeout(std::shared_ptr<IncomingTCPConnectionState>& state, bool write);

  void terminateClientConnection();
  void queueQuery(TCPQuery&& query);

  bool canAcceptNewQueries(const struct timeval& now);

  bool active() const
  {
    return d_ioState != nullptr;
  }

  std::string toString() const
  {
    ostringstream o;
    o << "Incoming TCP connection from "<<d_ci.remote.toStringWithPort()<<" over FD "<<d_handler.getDescriptor()<<", state is "<<(int)d_state<<", io state is "<<(d_ioState ? std::to_string((int)d_ioState->getState()) : "empty")<<", queries count is "<<d_queriesCount<<", current queries count is "<<d_currentQueriesCount<<", "<<d_queuedResponses.size()<<" queued responses, "<<d_activeConnectionsToBackend.size()<<" active connections to a backend";
    return o.str();
  }

  enum class State { doingHandshake, readingProxyProtocolHeader, waitingForQuery, readingQuerySize, readingQuery, sendingResponse, idle /* in case of XFR, we stop processing queries */ };

  std::map<std::shared_ptr<DownstreamState>, std::deque<std::shared_ptr<TCPConnectionToBackend>>> d_activeConnectionsToBackend;
  PacketBuffer d_buffer;
  std::deque<TCPResponse> d_queuedResponses;
  TCPClientThreadData& d_threadData;
  TCPResponse d_currentResponse;
  ConnectionInfo d_ci;
  ComboAddress d_origDest;
  ComboAddress d_proxiedRemote;
  ComboAddress d_proxiedDestination;
  TCPIOHandler d_handler;
  std::unique_ptr<IOStateHandler> d_ioState{nullptr};
  std::unique_ptr<std::vector<ProxyProtocolValue>> d_proxyProtocolValues{nullptr};
  struct timeval d_connectionStartTime;
  struct timeval d_handshakeDoneTime;
  struct timeval d_firstQuerySizeReadTime;
  struct timeval d_querySizeReadTime;
  struct timeval d_queryReadTime;
  size_t d_currentPos{0};
  size_t d_proxyProtocolNeed{0};
  size_t d_queriesCount{0};
  size_t d_currentQueriesCount{0};
  unsigned int d_remainingTime{0};
  uint16_t d_querySize{0};
  State d_state{State::doingHandshake};
  bool d_readingFirstQuery{true};
  bool d_isXFR{false};
  bool d_xfrStarted{false};
  bool d_proxyProtocolPayloadHasTLV{false};
  bool d_lastIOBlocked{false};
};
