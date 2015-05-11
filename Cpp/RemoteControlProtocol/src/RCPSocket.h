#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <array>
#include <atomic>
#include <unordered_map>
#include <map>
#include "random_access_queue.h"

#include <SFML/Network.hpp>

#include "Packet.h"


////////////////////////////////////////////////////////////////////////////////
// How it works.
//
//	*Threads*
//	There are 2 threads.
//	The first thread is the user's thread, on which calls to send and other
//	function are done. Nothing unusual.
//	The second thread is responsible for timing and tasks that need to be performed
//	even if there's no explicit user interaction (such as keepalives).
//	This threading scheme comes alive after the connection is established via either
//	accept or connect, and lives until disconnect is called.
//	
//	*Data flow*
//	Sending messages is done directly in send, nothing special.
//	However, recieving is done in the second thread, in an infinite loop.
//	The messages are then pushed into a queue, from which the recieve function
//	takes one out. The second thread also performs filtering and processing of
//	the messages. Protocol messages, such as keepalives are not pushed to the queue.
//	Acks are directly sent from this thread. If a batch number references a packet
//	that is not recieved yet, a place in the queue is reserved for it. The queue
//	is not committed until that packet arrives.
//
//	*Locking*
//	...
////////////////////////////////////////////////////////////////////////////////

class RcpSocket {
private:
	// RcpTester only exists for debug purposes
	friend class RcpTester;

	// RcpHeader is an internal helper structure for managing packet headers.
	struct RcpHeader {
		uint32_t sequenceNumber;
		uint32_t batchNumber;
		uint32_t flags;
		static std::array<unsigned char, 12> serialize(const RcpHeader& h);
		static RcpHeader deserialize(const void* data, size_t size);
		inline std::array<unsigned char, 12> serialize() const { return serialize(*this); }
	};
	// Flags that can be associated with a packet header.
	enum eFlags : uint32_t {
		SYN = 1, // connection requested
		ACK = 2, // acknowledged
		FIN = 4, // no more messages
		KEP = 8, // keep alive
		REL = 16, // reliable packet, send back ack
		CANCEL = 1u << 31, // special packet used to wake up selectors and cause cancellation of pending operation
	};

	// Event definitions
	enum eClosestEventType {
		ACK_RESEND,
		ACK_TIMEOUT,
		KEEPALIVE,
		RECV_TIMEOUT,
		RESERVE_TIMEOUT,
		RELOOP,
	};

	struct RecentPacketInfo;
	struct EventArgs {
		std::chrono::microseconds remaining;
		RecentPacketInfo* resendInfo;
	};

	// Internal states of the socket.
	enum eState {
		DISCONNECTED,
		CONNECTED,
		CLOSING,
	};

public:
	static const int AnyPort = 0;

	// --- Custructors & Destructor --- //
	RcpSocket();
	~RcpSocket();

	// --- Modifiers --- //
	// binding
	bool bind(uint16_t port);
	void unbind();
	bool isBound() const;
	// connection
	bool isConnected() const;
	std::string getRemoteAddress() const;
	uint16_t getRemotePort() const;
	uint16_t getLocalPort() const;
	// blocking, cancel
	void setBlocking(bool isBlocking);
	bool getBlocking() const;
	void cancel();

	// --- Connection setup --- //
	bool accept();
	bool connect(std::string address, uint16_t port);
	void disconnect();

	// --- Traffic --- //
	bool send(const void* data, size_t size, bool reliable);
	bool send(Packet& packet);
	bool receive(Packet& packet);
	
	// --- DEBUG!!! --- //
	std::string debug_PrintState();
	void debug_connect(std::string address, uint16_t port);
	void debug_kill();
	void debug_enableLog(bool value);
private:
	// --- Network resources --- //

	sf::UdpSocket socket; // communication socket
	sf::SocketSelector selector; // allows to wait for a certain time for this socket

	// --- IO thread --- //

	// This thread performs background socket communication
	std::thread ioThread;
	std::atomic_bool runIoThread;

	void startIoThread();
	void stopIoThread();
	void ioThreadFunction();

	// --- Traffic data structures --- //

	// Incoming packets	
	random_access_queue<std::pair<Packet, bool>> recvQueue; // recieved valid packets are put here
	std::condition_variable recvCondvar;	// notified when stuff is recieved

	// Incoming packet place reservation
	struct ReservedInfo {
		size_t index;
		std::chrono::steady_clock::time_point timestamp;
	};
	using ReservedMapT = std::map<uint32_t, ReservedInfo>;
	ReservedMapT recvReserved; // batch number and index-in-recvQueue of reserved places
	uint32_t remoteBatchNumReserved; // reliable packets having this or smaller batch number have space reserved or been already committed

	// Packets waiting to be ACK'd
	struct RecentPacketInfo {
		RcpHeader header;
		std::vector<uint8_t> data;
		std::chrono::steady_clock::time_point send;
		std::chrono::steady_clock::time_point lastResend;
	};
	using RecentPacketMapT = std::unordered_map < uint32_t, RecentPacketInfo> ; // batch num, info
	RecentPacketMapT recentPackets; // set of recently sent reliable packets waiting to be ACKed

	// --- Session description --- //
	// Local state
	eState state; // current state of the connection
	uint32_t localSeqNum; // keeps track of the local sequence number, increase for each packet
	uint32_t localBatchNum; // keeps track of the local batch number, refresh for each reliable packet sent

	// Remote partner's state
	uint32_t remoteSeqNum; // keeps track of the latest incoming packet's seqnum
	uint32_t remoteBatchNum; // keeps track of the latest reliable packet's batch num
	sf::IpAddress remoteAddress; // ip address of the remote partner
	uint16_t remotePort; // port of the remote partner
	
	// --- Miscallaneous --- //
	std::mutex socketMutex; // lock whenever accessing data shared b/w main & IO thread
	std::chrono::steady_clock::time_point timeLastSend; // the time of last packet send, including ACKs & KEPs
	std::chrono::steady_clock::time_point timeLastRecieved;

	bool isBlocking; // sets if calls block caller or return immediatly

	// Well, remove this shit from here and make it configurable and tidy
	static const unsigned TIMEOUT_TOTAL = 5000; // connection lost if no message for % ms
	static const unsigned TIMEOUT_SHORT = 200; // resend packet, resend kep, granularity of longer operations

	// --- Internal helper functions --- //
	bool sendEx(const void* data, size_t size, uint32_t flags); // send message with management of internal structures
	void reset(); // clean up data structures after a session
	void replyClose(); // perform closing procedure after getting a FIN
	std::vector<uint8_t> makePacket(const RcpHeader& header, const void* data, size_t size);
	bool decodeDatagram(const sf::Packet& packet, const sf::IpAddress& sender, uint16_t port, RcpHeader& rcpHeader, Packet& rcpPacket);
	eClosestEventType getNextEvent(EventArgs& args);

	// --- Cancellation --- //
	size_t cancelCallId;
	std::atomic<size_t> cancelNotify;

	// DEBUG!!!
	bool debugLog;
	static std::mutex debugLock;
	enum eDir {
		RECV,
		SEND,
	};
	void debugPrintMsg(RcpHeader, eDir);
	friend std::ostream& operator<<(std::ostream& os, RcpHeader h);
	void initDebug();
	unsigned color;
	static unsigned colorSt;
};








class RcpTester {
public:
	typedef RcpSocket::RcpHeader RcpHeader;
	enum eFlags {
		SYN = 1, // connection requested
		ACK = 2, // acknowledged
		FIN = 4, // no more messages
		KEP = 8, // keep alive
		REL = 16, // reliable packet, send back ack
	};

	RcpTester();

	bool bind(uint16_t localPort);
	bool send(RcpHeader header, void* data, size_t dataSize, std::string address, uint32_t port);
	bool receive(Packet& packet, RcpHeader& header);
private:
	sf::UdpSocket socket;
};