// =================================================================================================
// eModbus: Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to eModbus
//               MIT license - see license.md for details
// =================================================================================================
#include "ModbusClientTCPAsyncOne.h"
#define LOCAL_LOG_LEVEL LOG_LEVEL_VERBOSE
// #undef LOCAL_LOG_LEVEL
#include "Logging.h"

ModbusClientTCPAsyncOne::ModbusClientTCPAsyncOne(IPAddress address, uint16_t port, uint16_t queueLimit) :
  ModbusClient(),
  MTA_client(),
  MTA_timeout(DEFAULTTIMEOUT),
  MTA_idleTimeout(DEFAULTIDLETIME),
  MTA_qLimit(queueLimit),
  MTA_lastActivity(0),
  MTA_state(DISCONNECTED),
  MTA_host(address),
  MTA_port(port)
    {
      MTA_client.onConnect([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onConnect(); }, this);
      MTA_client.onDisconnect([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onDisconnect(); }, this);
      MTA_client.onError([](void* i, AsyncClient* c, int8_t error) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onError(c, error); }, this);
      MTA_client.onAck([](void* i, AsyncClient* c, size_t len, uint32_t time) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onAck(len, time); }, this);
      MTA_client.onData([](void* i, AsyncClient* c, void* data, size_t len) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onData(static_cast<uint8_t*>(data), len); }, this);
      MTA_client.onPoll([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPAsyncOne*>(i))->_onPoll(); }, this);

      // disable nagle algorithm ref Modbus spec
      MTA_client.setNoDelay(true);
    }

// Destructor: clean up queue, task etc.
ModbusClientTCPAsyncOne::~ModbusClientTCPAsyncOne() {
  MTA_client.close(true);
}

// Base syncRequest follows the same pattern
ModbusMessage ModbusClientTCPAsyncOne::syncRequestM(ModbusMessage msg, uint32_t token) {
  if (!msg) {
    ModbusMessage response;
    response.setError(msg.getServerID(), msg.getFunctionCode(), EMPTY_MESSAGE);
    return response;
  }

  _isResponseReady = false;
  _isRequestReady = true;
  _request = msg;

  connect();
  if (_state() != CONNECTED) {
    ModbusMessage response;
    response.setError(msg.getServerID(), msg.getFunctionCode(), IP_CONNECTION_FAILED);
    return response;
  }

  _send();
  auto _timeout = millis() + MTA_timeout;
  while (!_isResponseReady) {
    if (millis() >= _timeout) {
      ModbusMessage response;
      response.setError(msg.getServerID(), msg.getFunctionCode(), TIMEOUT);
      return response;
    }

    delay(10);
  }

  _isRequestReady = false;
  _isResponseReady = false;
  return _response;
}

// optionally manually connect to modbus server. Otherwise connection will be made upon first request
void ModbusClientTCPAsyncOne::connect() {
  if (_state() == CONNECTED) {
    LOG_D("already connected\n");
    return;
  }

  LOG_D("not connected, connecting\n");
  MTA_client.connect(MTA_host, MTA_port);
  int timeout = millis() + MTA_timeout;
  while (_state() != CONNECTED) {
    if (millis() >= timeout) {
      LOG_D("timeout %dms on connect\n", MTA_timeout);
      disconnect(true);
      return;
    }

    delay(10);
  }
}

// manually disconnect from modbus server. Connection will also auto close after idle time
 void ModbusClientTCPAsyncOne::disconnect(bool force) {
  LOG_D("disconnecting\n");

 // _isResponseReady = false;
  MTA_client.close(force);
}

// Set timeout value
void ModbusClientTCPAsyncOne::setTimeout(uint32_t timeout) {
  MTA_timeout = timeout;
}

// Set idle timeout value (time before connection auto closes after being idle)
void ModbusClientTCPAsyncOne::setIdleTimeout(uint32_t timeout) {
  MTA_idleTimeout = timeout;
}

// Base addRequest for preformatted ModbusMessage and last set target
Error ModbusClientTCPAsyncOne::addRequestM(ModbusMessage msg, uint32_t token) {
  return SUCCESS;
}

ModbusClientTCPAsyncOne::ClientState ModbusClientTCPAsyncOne::_state() {
  switch (MTA_client.state()) {
    case 4: 
      return CONNECTED;
    case 2: 
    case 3:
      return CONNECTING;
    default:
      return DISCONNECTED;
  }
}

void ModbusClientTCPAsyncOne::_onConnect() {
  LOG_D("connected\n");
  MTA_lastActivity = millis();
}

void ModbusClientTCPAsyncOne::_onDisconnect() {
  LOG_D("disconnected\n");

  ModbusMessage response;
  response.setError(0, 0, IP_CONNECTION_FAILED);

  if (_isResponseReady) LOG_E("was already ready\n") ;

  _isRequestReady = false;
  _isResponseReady = true;
  _response = response;
}

void ModbusClientTCPAsyncOne::_onAck(size_t len, uint32_t time) {
  MTA_lastActivity = millis();
}

void ModbusClientTCPAsyncOne::_onPoll() {
  if (millis() - MTA_lastActivity > MTA_idleTimeout) {
    Serial.println("_onPoll: disconnect");
    disconnect();
  }
}


bool ModbusClientTCPAsyncOne::_send() {
  MTA_lastActivity = millis();

  if (!_isRequestReady) {
    return false;
  }

  if(!MTA_client.connected() || !MTA_client.canSend()) {
    LOG_E("can't send\n");
    return false;
  }

  std::lock_guard<std::mutex> lck(_sendLock);
  
  // Write TCP header first
  ModbusTCPhead head;
  head.transactionID = _transactionID++;
  head.len = _request.size();

  // check if TCP client is able to send
  if (MTA_client.space() < ((uint32_t)_request.size() + 6)) {
    return false;
  }
  

  MTA_client.add(reinterpret_cast<const char *>((const uint8_t *)(head)), 6, ASYNC_WRITE_FLAG_COPY);
  
  // Request comes next
  MTA_client.add(reinterpret_cast<const char*>(_request.data()), _request.size(), ASYNC_WRITE_FLAG_COPY);
  
  // done
  MTA_client.send();
  LOG_D("request sent (msgid:%d)\n", head.transactionID);

  _isRequestReady = false;
  return true;
}

void ModbusClientTCPAsyncOne::_onError(AsyncClient* c, int8_t error) {
  // onDisconnect will alse be called, so nothing to do here
  LOG_W("TCP error: %s\n", c->errorToString(error));
  disconnect(true);
}

void ModbusClientTCPAsyncOne::_onData(uint8_t* data, size_t length) {
  LOG_D("packet received (len:%d), state=%d\n", length, _state());
  // reset idle timeout
  MTA_lastActivity = millis();

  if (length) {
    LOG_D("parsing (len:%d)\n", length + 1);
  }

  while (length > 0) {
    uint16_t transactionID = 0;
    uint16_t protocolID = 0;
    uint16_t messageLength = 0;

    // MBAP header is 6 bytes, we can't do anything with less
    // total message should fit MBAP plus remaining bytes (in data[4], data[5])
    if (length > 6) {
      transactionID = (data[0] << 8) | data[1];
      protocolID = (data[2] << 8) | data[3];
      messageLength = (data[4] << 8) | data[5];
      if (protocolID == 0 && length >= (uint32_t)messageLength + 6 && messageLength < 256) {
        _response.resize(messageLength);
        _response.clear();
        _response.add(&data[6], messageLength);
        LOG_D("packet validated (len:%d)\n", messageLength);

        // on next iteration: adjust remaining length and pointer to data
        length -= 6 + messageLength;
        data += 6 + messageLength;
        _isResponseReady = true;
      }
    }
  }
  
  _send();
}