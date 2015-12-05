#include "stdafx.h"
#ifndef NOCLOUD
#include "proxyclient.h"
#include "request.hpp"
#include "reply.hpp"
#include "request_parser.hpp"
#include "../main/SQLHelper.h"
#include "../webserver/Base64.h"
#include "../tcpserver/TCPServer.h"

extern std::string szAppVersion;

#define TIMEOUT 60

namespace http {
	namespace server {

		CProxySharedData sharedData;

		CProxyClient::CProxyClient(boost::asio::io_service& io_service, boost::asio::ssl::context& context, http::server::cWebem *webEm)
			: _socket(io_service, context),
			_io_service(io_service),
			doStop(false),
			b_Connected(false),
			we_locked_prefs_mutex(false),
			timeout_(TIMEOUT),
			timer_(io_service, boost::posix_time::seconds(TIMEOUT))
		{
			_apikey = "";
			_password = "";
			_allowed_subsystems = 0;
			m_sql.GetPreferencesVar("MyDomoticzUserId", _apikey);
			m_sql.GetPreferencesVar("MyDomoticzPassword", _password);
			m_sql.GetPreferencesVar("MyDomoticzSubsystems", _allowed_subsystems);
			if (_password != "") {
				_password = base64_decode(_password);
			}
			if (_apikey == "" || _password == "" || _allowed_subsystems == 0) {
				doStop = true;
				return;
			}
			m_writeThread = new boost::thread(boost::bind(&CProxyClient::WriteThread, this));
			m_pWebEm = webEm;
			m_pDomServ = NULL;
			Reconnect();
		}

		void CProxyClient::WriteSlaveData(const std::string &token, const char *pData, size_t Length)
		{
			/* data from slave to master */
			CValueLengthPart parameters;

			parameters.AddPart(token);
			parameters.AddPart((void *)pData, Length);

			MyWrite(PDU_SERV_RECEIVE, parameters);
		}

		void CProxyClient::WriteMasterData(const std::string &token, const char *pData, size_t Length)
		{
			/* data from master to slave */
			CValueLengthPart parameters;

			parameters.AddPart(token);
			parameters.AddPart((void *)pData, Length);

			MyWrite(PDU_SERV_SEND, parameters);
		}

		void CProxyClient::Reconnect()
		{

			std::string address = "my.domoticz.com";
			std::string port = "9999";

			if (we_locked_prefs_mutex) {
				// avoid deadlock if we got a read or write error in between handle_handshake() and HandleAuthresp()
				we_locked_prefs_mutex = false;
				sharedData.LockPrefsMutex();
			}
			b_Connected = false;
			boost::asio::ip::tcp::resolver resolver(_io_service);
			boost::asio::ip::tcp::resolver::query query(address, port);
			boost::asio::ip::tcp::resolver::iterator iterator = resolver.resolve(query);
			boost::asio::ip::tcp::endpoint endpoint = *iterator;
			_socket.lowest_layer().async_connect(endpoint,
				boost::bind(&CProxyClient::handle_connect, this,
					boost::asio::placeholders::error, iterator));
		}

		void CProxyClient::handle_connect(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
		{
			if (!error)
			{
				_socket.async_handshake(boost::asio::ssl::stream_base::client,
					boost::bind(&CProxyClient::handle_handshake, this,
						boost::asio::placeholders::error));
			}
			else if (endpoint_iterator != boost::asio::ip::tcp::resolver::iterator())
			{
				_socket.lowest_layer().close();
				boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
				_socket.lowest_layer().async_connect(endpoint,
					boost::bind(&CProxyClient::handle_connect, this,
						boost::asio::placeholders::error, ++endpoint_iterator));
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Connect failed, reconnecting: %s", error.message().c_str());
				if (!doStop) {
					boost::this_thread::sleep_for(boost::chrono::seconds(10));
					Reconnect();
				}
			}
		}

		void CProxyClient::handle_timeout(const boost::system::error_code& error)
		{
			if (error != boost::asio::error::operation_aborted) {
				_log.Log(LOG_ERROR, "PROXY: timeout occurred, reconnecting");
				_socket.lowest_layer().close(); // should induce a reconnect in handle_read with error != 0
			}
		}

		void CProxyClient::WriteThread()
		{
			std::vector<boost::asio::const_buffer> _writebuf;
			ProxyPdu *writePdu;

			while ((writePdu = writeQ.Take()) != NULL) {
				_writebuf.clear();
				_writebuf.push_back(boost::asio::buffer(writePdu->content(), writePdu->length()));
				try {
					boost::asio::write(_socket, _writebuf);
#ifdef WIN32
					boost::this_thread::sleep_for(boost::chrono::milliseconds(100)); // for peace of mind of openssl (workaround for openssl bug)
#endif
				}
				catch (std::exception& e) {
					if (doStop) {
						// exit thread
						return;
					}
					_log.Log(LOG_ERROR, "PROXY: Write error, reconnecting: %s", e.what());
					// Initiate graceful connection closure.
					_socket.lowest_layer().close();
					// we are disconnected, reconnect
					boost::this_thread::sleep_for(boost::chrono::seconds(10));
					Reconnect();
				}
				delete writePdu;
			}
		}

		void CProxyClient::MyWrite(pdu_type type, CValueLengthPart &parameters)
		{
			if (!b_Connected) {
				return;
			}
			ProxyPdu *writePdu = new ProxyPdu(type, &parameters);
			writeQ.Put(writePdu);
#if 0
			boost::mutex::scoped_lock lock(write_mutex);
			std::vector<boost::asio::const_buffer> _writebuf;
			_writebuf.clear();
			ProxyPdu writePdu(type, parameters);

			_writebuf.push_back(boost::asio::buffer(writePdu.content(), writePdu.length()));
			boost::asio::write(_socket, _writebuf);
#endif
		}

		void CProxyClient::LoginToService()
		{
			std::string instanceid = sharedData.GetInstanceId();
			const long protocol_version = 2;

			// start read thread
			ReadMore();
			// send authenticate pdu
			CValueLengthPart parameters;
			parameters.AddPart(_apikey);
			parameters.AddPart(instanceid);
			parameters.AddPart(_password);
			parameters.AddPart(szAppVersion);
			parameters.AddLong(_allowed_subsystems);
			parameters.AddLong(protocol_version);
			MyWrite(PDU_AUTHENTICATE, parameters);
		}

		void CProxyClient::handle_handshake(const boost::system::error_code& error)
		{
			if (!error)
			{
				// lock until we have a valid api id
				sharedData.UnlockPrefsMutex();
				b_Connected = true;
				we_locked_prefs_mutex = true;
				LoginToService();
			}
			else
			{
				if (doStop) {
					return;
				}
				_log.Log(LOG_ERROR, "PROXY: Handshake failed, reconnecting: %s", error.message().c_str());
				_socket.lowest_layer().close();
				boost::this_thread::sleep_for(boost::chrono::seconds(10));
				Reconnect();
			}
		}

		void CProxyClient::ReadMore()
		{
			// read chunks of max 4 KB
			boost::asio::streambuf::mutable_buffers_type buf = _readbuf.prepare(4096);

			// set timeout timer
			timer_.expires_from_now(boost::posix_time::seconds(timeout_));
			timer_.async_wait(boost::bind(&CProxyClient::handle_timeout, this, boost::asio::placeholders::error));

			_socket.async_read_some(buf,
				boost::bind(&CProxyClient::handle_read, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
				);
		}

		void CProxyClient::GetRequest(const std::string originatingip, boost::asio::mutable_buffers_1 _buf, http::server::reply &reply_)
		{
			/// The parser for the incoming request.
			http::server::request_parser request_parser_;
			http::server::request request_;

			boost::tribool result;
			try
			{
				size_t bufsize = boost::asio::buffer_size(_buf);
				const char *begin = boost::asio::buffer_cast<const char*>(_buf);
				boost::tie(result, boost::tuples::ignore) = request_parser_.parse(
					request_, begin, begin + bufsize);
			}
			catch (...)
			{
				_log.Log(LOG_ERROR, "PROXY: Exception during request parsing");
			}

			if (result)
			{
				request_.host = originatingip;
				m_pWebEm->myRequestHandler.handle_request(request_, reply_);
			}
			else if (!result)
			{
				reply_ = http::server::reply::stock_reply(http::server::reply::bad_request);
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Could not parse request.");
			}
		}

		std::string CProxyClient::GetResponseHeaders(const http::server::reply &reply_)
		{
			std::string result = "";
			for (std::size_t i = 0; i < reply_.headers.size(); ++i) {
				result += reply_.headers[i].name;
				result += ": ";
				result += reply_.headers[i].value;
				result += "\r\n";
			}
			return result;
		}

		void CProxyClient::HandleRequest(ProxyPdu *pdu)
		{
			// response variables
			boost::asio::mutable_buffers_1 _buf(NULL, 0);
			/// The reply to be sent back to the client.
			http::server::reply reply_;
			// we number the request, because we can send back asynchronously
			long requestid;

			// get common parts for the different subsystems
			CValueLengthPart part(pdu);

			// request parts
			std::string originatingip, requesturl, requestheaders, requestbody;
			long subsystem;
			std::string responseheaders;
			std::string request;
			CValueLengthPart parameters; // response parameters

			if (!part.GetNextPart(originatingip)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid request");
				return;
			}
			if (!part.GetNextLong(subsystem)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid request");
				return;
			}

			switch (subsystem) {
			case SUBSYSTEM_HTTP:
				// "normal web request", get parameters
				if (!part.GetNextPart(requesturl)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					return;
				}
				if (!part.GetNextPart(requestheaders)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					return;
				}
				if (!part.GetNextPart(requestbody, false)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					return;
				}
				if (!part.GetNextLong(requestid)) {
					requestid = 0;
				}

				if (requestbody.size() > 0) {
					request = "POST ";
				}
				else {
					request = "GET ";
				}
				request += requesturl;
				request += " HTTP/1.1\r\n";
				request += requestheaders;
				request += "\r\n";
				request += requestbody;

				_buf = boost::asio::buffer((void *)request.c_str(), request.length());

				// todo: shared data -> AddConnectedIp(originatingip);

				GetRequest(originatingip, _buf, reply_);
				// assemble response
				responseheaders = GetResponseHeaders(reply_);


				parameters.AddLong(reply_.status);
				parameters.AddPart(responseheaders);
				parameters.AddPart(reply_.content);
				parameters.AddLong(requestid);

				// send response to proxy
				MyWrite(PDU_RESPONSE, parameters);
				break;
			default:
				// unknown subsystem
				_log.Log(LOG_ERROR, "PROXY: Got pdu for unknown subsystem %d.", subsystem);
				break;
			}
		}

		void CProxyClient::HandleAssignkey(ProxyPdu *pdu)
		{
			// get our new api key
			CValueLengthPart parameters(pdu);
			std::string newapi;

			if (!parameters.GetNextPart(newapi)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid request while obtaining API key");
				return;
			}
			_log.Log(LOG_STATUS, "PROXY: We were assigned an instance id: %s.\n", newapi.c_str());
			sharedData.SetInstanceId(newapi);
			// re-login with the new instance id
			LoginToService();
		}

		void CProxyClient::HandleEnquire(ProxyPdu *pdu)
		{
			// assemble response
			CValueLengthPart parameters;

			// send response to proxy
			MyWrite(PDU_ENQUIRE, parameters);
		}

		void CProxyClient::HandleServRosterInd(ProxyPdu *pdu)
		{
			CValueLengthPart part(pdu);
			std::string c_slave;

			_log.Log(LOG_STATUS, "PROXY: SERV_ROSTERIND pdu received.");
			if (!part.GetNextPart(c_slave)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_ROSTERIND pdu");
			}
			_log.Log(LOG_STATUS, "PROXY: Looking for slave %s", c_slave.c_str());
			DomoticzTCP *slave = sharedData.findSlaveById(c_slave);
			if (slave) {
				_log.Log(LOG_STATUS, "PROXY: SetConnected(true)");
				slave->SetConnected(true);
			}
		}

		void CProxyClient::HandleServDisconnect(ProxyPdu *pdu)
		{
			CValueLengthPart part(pdu);
			std::string tokenparam;
			long reason;
			bool success;

			_log.Log(LOG_STATUS, "PROXY: SERV_DISCONNECT pdu received.");
			if (!part.GetNextPart(tokenparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_DISCONNECT pdu");
			}
			if (!part.GetNextLong(reason)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_DISCONNECT pdu");
			}
			// see if we are slave
			success = m_pDomServ->OnDisconnect(tokenparam);
			_log.Log(LOG_STATUS, "PROXY: Master disconnected: %s", success ? "success" : "not found");
			// see if we are master
			DomoticzTCP *slave = sharedData.findSlaveConnection(tokenparam);
			if (slave && slave->isConnected()) {
				_log.Log(LOG_STATUS, "PROXY: Stopping slave connection.");
				slave->SetConnected(false);
			}
		}

		void CProxyClient::HandleServConnect(ProxyPdu *pdu)
		{
			CValueLengthPart part(pdu);
			CValueLengthPart parameters;
			long authenticated, version;
			std::string tokenparam, usernameparam, passwordparam, ipparam;
			std::string reason = "";

			_log.Log(LOG_NORM, "PROXY: SERV_CONNECT pdu received.");
			if (!part.GetNextPart(tokenparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECT pdu");
			}
			if (!part.GetNextPart(usernameparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECT pdu");
			}
			if (!part.GetNextPart(passwordparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECT pdu");
			}
			if (!part.GetNextLong(version)) {
				version = 1;
			}
			if (!part.GetNextPart(ipparam)) {
				ipparam = "unknown";
			}
			sharedData.AddConnectedServer(ipparam);
			if (version > 3) {
				authenticated = 0;
				reason = "Version not supported";
			}
			else {
				authenticated = m_pDomServ->OnNewConnection(tokenparam, usernameparam, passwordparam) ? 1 : 0;
				reason = authenticated ? "Success" : "Invalid user/password";
			}
			parameters.AddPart(tokenparam);
			parameters.AddPart(sharedData.GetInstanceId());
			parameters.AddLong(authenticated);
			parameters.AddPart(reason);
			MyWrite(PDU_SERV_CONNECTRESP, parameters);
		}

		void CProxyClient::HandleServConnectResp(ProxyPdu *pdu)
		{
			CValueLengthPart part(pdu);
			long authenticated;
			std::string tokenparam, instanceparam, reason;

			_log.Log(LOG_NORM, "PROXY: SERV_CONNECTRESP pdu received.");
			if (!part.GetNextPart(tokenparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECTRESP pdu");
			}
			if (!part.GetNextPart(instanceparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECTRESP pdu");
			}
			if (!part.GetNextLong(authenticated)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECTRESP pdu");
			}
			if (!part.GetNextPart(reason)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_CONNECTRESP pdu");
			}
			if (!authenticated) {
				_log.Log(LOG_ERROR, "PROXY: Could not log in to slave: %s", reason.c_str());
			}
			DomoticzTCP *slave = sharedData.findSlaveConnection(instanceparam);
			if (slave) {
				slave->Authenticated(tokenparam, authenticated == 1);
			}
		}

		void CProxyClient::HandleServSend(ProxyPdu *pdu) {
			/* data from master to slave */
			CValueLengthPart part(pdu);
			size_t datalen;
			unsigned char *data;
			std::string tokenparam;
			bool success;

			if (!part.GetNextPart(tokenparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_SEND pdu");
			}
			if (!part.GetNextPart((void **)&data, &datalen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_SEND pdu");
			}
			success = m_pDomServ->OnIncomingData(tokenparam, data, datalen);
			free(data);
			if (!success) {
				SendServDisconnect(tokenparam, 1);
			}
		}

		void CProxyClient::HandleServReceive(ProxyPdu *pdu)
		{
			/* data from slave to master */
			CValueLengthPart part(pdu);
			std::string tokenparam;
			unsigned char *data;
			size_t datalen;

			if (!part.GetNextPart(tokenparam)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_RECEIVE pdu");
			}
			if (!part.GetNextPart((void **)&data, &datalen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid SERV_RECEIVE pdu");
			}
			DomoticzTCP *slave = sharedData.findSlaveConnection(tokenparam);
			if (slave && slave->isConnected()) {
				slave->FromProxy(data, datalen);
			}
			free(data);
		}

		void CProxyClient::SendServDisconnect(const std::string &token, int reason)
		{
			CValueLengthPart parameters;

			parameters.AddPart(token);
			parameters.AddLong(reason);
			MyWrite(PDU_SERV_DISCONNECT, parameters);
		}

		void CProxyClient::HandleAuthresp(ProxyPdu *pdu)
		{
			// get auth response (0 or 1)
			long auth;
			std::string reason;
			CValueLengthPart part(pdu);

			// unlock prefs mutex
			we_locked_prefs_mutex = false;
			sharedData.UnlockPrefsMutex();

			if (!part.GetNextLong(auth)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid pdu while receiving authentication response");
				return;
			}
			if (!part.GetNextPart(reason)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid pdu while receiving authentication response");
				return;
			}
			_log.Log(LOG_STATUS, "PROXY: Authenticate result: %s.", auth ? "success" : reason.c_str());
			if (!auth) {
				Stop();
				return;
			}
		}

		void CProxyClient::PduHandler(ProxyPdu &pdu)
		{
			switch (pdu._type) {
			case PDU_REQUEST:
				if (_allowed_subsystems & SUBSYSTEM_HTTP) {
					HandleRequest(&pdu);
				}
				else {
					_log.Log(LOG_ERROR, "PROXY: HTTP access disallowed, denying request.");
				}
				break;
			case PDU_ASSIGNKEY:
				HandleAssignkey(&pdu);
				break;
			case PDU_ENQUIRE:
				HandleEnquire(&pdu);
				break;
			case PDU_AUTHRESP:
				HandleAuthresp(&pdu);
				break;
			case PDU_SERV_CONNECT:
				/* incoming connect from master */
				if (_allowed_subsystems & SUBSYSTEM_SHAREDDOMOTICZ) {
					HandleServConnect(&pdu);
				}
				else {
					_log.Log(LOG_ERROR, "PROXY: Shared Server access disallowed, denying connect request.");
				}
				break;
			case PDU_SERV_DISCONNECT:
				if (_allowed_subsystems & SUBSYSTEM_SHAREDDOMOTICZ) {
					HandleServDisconnect(&pdu);
				}
				else {
					_log.Log(LOG_ERROR, "PROXY: Shared Server access disallowed, denying disconnect request.");
				}
				break;
			case PDU_SERV_CONNECTRESP:
				/* authentication result from slave */
				HandleServConnectResp(&pdu);
				break;
			case PDU_SERV_RECEIVE:
				/* data from slave to master */
				if (_allowed_subsystems & SUBSYSTEM_SHAREDDOMOTICZ) {
					HandleServReceive(&pdu);
				}
				else {
					_log.Log(LOG_ERROR, "PROXY: Shared Server access disallowed, denying receive data request.");
				}
				break;
			case PDU_SERV_SEND:
				/* data from master to slave */
				HandleServSend(&pdu);
				break;
			case PDU_SERV_ROSTERIND:
				/* the slave that we want to connect to is back online */
				HandleServRosterInd(&pdu);
				break;
			default:
				_log.Log(LOG_ERROR, "PROXY: pdu type: %d not expected.", pdu._type);
				break;
			}
		}

		void CProxyClient::handle_read(const boost::system::error_code& error, size_t bytes_transferred)
		{
			// data read, no need for timeouts anymore
			timer_.cancel();
			if (!error)
			{
				_readbuf.commit(bytes_transferred);
				const char *data = boost::asio::buffer_cast<const char*>(_readbuf.data());
				ProxyPdu pdu(data, _readbuf.size());
				if (pdu.Disconnected()) {
					// todo
					ReadMore();
					return;
				}
				PduHandler(pdu);
				_readbuf.consume(pdu.length() + 9); // 9 is header size
				ReadMore();
			}
			else
			{
				if (doStop) {
					return;
				}
				if (error.value() == 0x140000DB) {
					// short read
					_log.Log(LOG_ERROR, "PROXY: Read failed, , short read");
					// Initiate graceful connection closure.
					_socket.lowest_layer().close();
					// we are disconnected, reconnect
					Reconnect();
					return;
				}
				_log.Log(LOG_ERROR, "PROXY: Read failed, code = %d. Reconnecting: %s", error.value(), error.message().c_str());
				// Initiate graceful connection closure.
				_socket.lowest_layer().close();
				// we are disconnected, reconnect
				boost::this_thread::sleep_for(boost::chrono::seconds(10));
				Reconnect();
			}
		}

		bool CProxyClient::SharedServerAllowed()
		{
			return ((_allowed_subsystems & SUBSYSTEM_SHAREDDOMOTICZ) > 0);
		}

		void CProxyClient::Stop()
		{
			if (we_locked_prefs_mutex) {
				we_locked_prefs_mutex = false;
				sharedData.UnlockPrefsMutex();
			}

			doStop = true;
			// signal end of WriteThread
			writeQ.Put(NULL);
			m_writeThread->join();
			_socket.lowest_layer().close();
		}

		void CProxyClient::SetSharedServer(tcp::server::CTCPServerProxied *domserv)
		{
			m_pDomServ = domserv;
		}

		CProxyClient::~CProxyClient()
		{
		}

		CProxyManager::CProxyManager(const std::string& doc_root, http::server::cWebem *webEm, tcp::server::CTCPServer *domServ)
		{
			proxyclient = NULL;
			m_pWebEm = webEm;
			m_pDomServ = domServ;
			m_thread = NULL;
		}

		CProxyManager::~CProxyManager()
		{
			if (m_thread) {
				m_thread->join();
			}
			if (proxyclient) delete proxyclient;
		}

		int CProxyManager::Start(bool first)
		{
			_first = first;
			m_thread = new boost::thread(boost::bind(&CProxyManager::StartThread, this));
			return 1;
		}

		void CProxyManager::StartThread()
		{
			try {
				boost::asio::ssl::context ctx(io_service, boost::asio::ssl::context::sslv23);
				ctx.set_verify_mode(boost::asio::ssl::verify_none);

				proxyclient = new CProxyClient(io_service, ctx, m_pWebEm);
				if (_first && proxyclient->SharedServerAllowed()) {
					m_pDomServ->StartServer(proxyclient);
				}
				proxyclient->SetSharedServer(m_pDomServ->GetProxiedServer());

				io_service.run();
			}
			catch (std::exception& e)
			{
				_log.Log(LOG_ERROR, "PROXY: StartThread(): Exception: %s", e.what());
			}
		}

		void CProxyManager::Stop()
		{
			proxyclient->Stop();
			io_service.stop();
			m_thread->interrupt();
			m_thread->join();
		}

		CProxyClient *CProxyManager::GetProxyForMaster(DomoticzTCP *master) {
			sharedData.AddTCPClient(master);
			return proxyclient;
		}

		void CProxyClient::ConnectToDomoticz(std::string instancekey, std::string username, std::string password, DomoticzTCP *master, int version)
		{
			CValueLengthPart parameters;

			parameters.AddPart(instancekey);
			parameters.AddPart(username);
			parameters.AddPart(password);
			parameters.AddLong(version);
			MyWrite(PDU_SERV_CONNECT, parameters);
		}

		void CProxyClient::DisconnectFromDomoticz(const std::string &token, DomoticzTCP *master)
		{
			CValueLengthPart parameters;
			int reason = 2;

			parameters.AddPart(token);
			parameters.AddLong(reason);
			MyWrite(PDU_SERV_DISCONNECT, parameters);
			DomoticzTCP *slave = sharedData.findSlaveConnection(token);
			if (slave) {
				_log.Log(LOG_STATUS, "PROXY: Stopping slave.");
				slave->SetConnected(false);
			}
		}

		void CProxySharedData::SetInstanceId(std::string instanceid)
		{
			_instanceid = instanceid;
			m_sql.UpdatePreferencesVar("MyDomoticzInstanceId", _instanceid);
		}

		std::string CProxySharedData::GetInstanceId()
		{
			m_sql.GetPreferencesVar("MyDomoticzInstanceId", _instanceid);
			return _instanceid;
		}

		void CProxySharedData::LockPrefsMutex()
		{
			// todo: make this a boost::condition?
			prefs_mutex.lock();
		}

		void CProxySharedData::UnlockPrefsMutex()
		{
			prefs_mutex.unlock();
		}

		bool CProxySharedData::AddConnectedIp(std::string ip)
		{
			if (connectedips_.find(ip) == connectedips_.end())
			{
				//ok, this could get a very long list when running for years
				connectedips_.insert(ip);
				_log.Log(LOG_STATUS, "PROXY: Incoming connection from: %s", ip.c_str());
				return true;
			}
			return false;
		}

		bool CProxySharedData::AddConnectedServer(std::string ip)
		{
			if (connectedservers_.find(ip) == connectedservers_.end())
			{
				//ok, this could get a very long list when running for years
				connectedservers_.insert(ip);
				_log.Log(LOG_STATUS, "PROXY: Incoming Domoticz connection from: %s", ip.c_str());
				return true;
			}
			return false;
		}

		void CProxySharedData::AddTCPClient(DomoticzTCP *client)
		{
			for (std::vector<DomoticzTCP *>::iterator it = TCPClients.begin(); it != TCPClients.end(); it++) {
				if ((*it) == client) {
					return;
				}
			}
			int size = TCPClients.size();
			TCPClients.resize(size + 1);
			TCPClients[size] = client;
		}

		void CProxySharedData::RemoveTCPClient(DomoticzTCP *client)
		{
			// fast remove from vector
			std::vector<DomoticzTCP *>::iterator it = std::find(TCPClients.begin(), TCPClients.end(), client);
			if (it != TCPClients.end()) {
				using std::swap;
				// swap the one to be removed with the last element
				// and remove the item at the end of the container
				// to prevent moving all items after 'client' by one
				swap(*it, TCPClients.back());
				TCPClients.pop_back();
			}
		}

		DomoticzTCP *CProxySharedData::findSlaveConnection(const std::string &token)
		{
			for (unsigned int i = 0; i < TCPClients.size(); i++) {
				if (TCPClients[i]->CompareToken(token)) {
					return TCPClients[i];
				}
			}
			return NULL;
		}

		DomoticzTCP *CProxySharedData::findSlaveById(const std::string &instanceid)
		{
			for (unsigned int i = 0; i < TCPClients.size(); i++) {
				if (TCPClients[i]->CompareId(instanceid)) {
					return TCPClients[i];
				}
			}
			return NULL;
		}

		void CProxySharedData::RestartTCPClients()
		{
			for (unsigned int i = 0; i < TCPClients.size(); i++) {
				TCPClients[i]->ConnectInternalProxy();
			}
		}

		void CProxySharedData::StopTCPClients()
		{
#if 0
			for (unsigned int i = 0; i < TCPClients.size(); i++) {
				TCPClients[i]->StopHardwareProxy();
			}
#endif
		}
	}
}
#endif