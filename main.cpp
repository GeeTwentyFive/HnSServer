#include <unordered_map>
#include <iostream>
#include <string>
#include <experimental/scope>
#ifdef _WIN32
#include <windows.h>
#endif
#include <thread>

#define ENET_IMPLEMENTATION
#include "libs/enet.h"


#define DEFAULT_PORT 55555
#define MAX_PLAYERS 8


#pragma pack(1)
typedef struct {
	float x;
	float y;
	float z;
} Vec3;

enum PlayerStateFlags {
	ALIVE = 0,
	IS_SEEKER = 1,
	JUMPED = 2,
	WALLJUMPED = 4,
	SLIDING = 8,
	HOOKED = 16,
	FLASHLIGHT = 32
};

#pragma pack(1)
typedef struct {
	Vec3 pos;
	float yaw;
	float pitch;
	char player_state_flags; // PlayerStateFlags bitmask
	Vec3 hook_point;
} PlayerState;

#pragma pack(1)
typedef struct {
	float seek_time;
	char last_alive_rounds;
	unsigned char points;
} PlayerStats;


enum PacketType : char {
	PLAYER_CONNECTED,
	PLAYER_SYNC,
	PLAYER_HIDER_CAUGHT,
	PLAYER_STATS,
	PLAYER_DISCONNECTED,

	CONTROL_MAP_DATA,
	CONTROL_GAME_START,
	CONTROL_GAME_END
};

#pragma region PACKETS

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_CONNECTED;
	enet_uint8 connected_player_id;
} PlayerConnectedPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_SYNC;
	enet_uint8 player_id;
	PlayerState player_state;
} PlayerSyncPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_HIDER_CAUGHT;
	enet_uint8 player_id;
	enet_uint8 caught_hider_id;
} HiderCaughtPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_STATS;
	enet_uint8 player_id;
	PlayerStats player_stats;
} PlayerStatsPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_DISCONNECTED;
	enet_uint8 disconnected_player_id;
} PlayerDisconnectedPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_MAP_DATA;
	uint32_t map_data_size;
} MapDataControlPacketHeader;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_GAME_START;
} GameStartControlPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_GAME_END;
} GameEndControlPacketData;

#pragma endregion PACKETS


ENetHost* server;

std::unordered_map<enet_uint8, PlayerState> player_states;
std::unordered_map<enet_uint8, PlayerStats> player_stats;

std::string map_data;
enet_uint8 current_seeker_id = -1;


static inline void HandleReceive(
	enet_uint8 player_id,
	ENetPacket* packet
) {
	switch (*((PacketType*)(packet->data + 0))) {
		case PacketType::PLAYER_SYNC:
		{
			enet_uint8 player_id = *((enet_uint8*)(
				packet->data + offsetof(PlayerSyncPacketData, player_id)
			));
			player_states[player_id] = *((PlayerState*)(
				packet->data + offsetof(PlayerSyncPacketData, player_state)
			));

			enet_host_broadcast(server, 0, packet);
		}
		break;

		case PacketType::PLAYER_HIDER_CAUGHT:
		{
			// TODO:
			// - Check if not seeker: return;
			// - if one hider left:
			//	- set seeker's seek_time stat
			//	- increment caught hider's last_alive_rounds stat
			//	- if all players have been seekers:
			//		- calculate points stat for all players
			//		- PLAYER_STATS (reliable)
			//		- CONTROL+GAME_END (reliable)
			//		- enet_host_flush() -> enet_host_service() for 1000ms
			//		- exit()
			//	- else:
			//		- increment current_seeker_id to next player
			//		- set players positions to spawns
			//		- ^ broadcast all player states to all players (reliable)
			//		- set alive for all players and is_seeker for the current_seeker_id player
			//		- ^ broadcast all player states to all players (reliable)
			// - else:
			//	- set caught hider's ALIVE state flag to 0

			enet_host_broadcast(server, 0, packet);
		}
		break;

		default: break;
	}
}


int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cout << "USAGE: <PATH/TO/MAP.json> [PORT]" << std::endl;
		return 0;
	}

	std::string map_path = argv[1];
	int port = (argc >= 3) ? std::stoi(argv[2]) : DEFAULT_PORT;

	// TODO: Load map data from file

	if (!enet_initialize()) throw std::runtime_error("Failed to initialize ENet");
	auto _cleanup0 = std::experimental::scope_exit(enet_deinitialize);

	ENetAddress address = {0};
	address.host = ENET_HOST_ANY;
	address.port = port;
	server = enet_host_create(&address, MAX_PLAYERS, 1, 0, 0);
	if (server == nullptr) throw std::runtime_error("Failed to create ENet server");
	auto _cleanup1 = std::experimental::scope_exit([](){enet_host_destroy(server);});

	std::cout << "Server started on port " << port << std::endl;

	#ifdef _WIN32
	timeBeginPeriod(1);
	auto _cleanup_windows = std::experimental::scope_exit([](){timeEndPeriod(1);});
	#endif

	ENetEvent event;
	for (;;) {
		while (enet_host_service(server, &event, 0) > 0) {
			switch (event.type) {
				case ENET_EVENT_TYPE_CONNECT:
				{
					std::cout
					<< "Client "
					<< event.peer->incomingPeerID
					<< "connected"
					<< std::endl;

					PlayerConnectedPacketData pcp_data;
					pcp_data.connected_player_id = event.peer->incomingPeerID;
					ENetPacket* player_connected_packet = enet_packet_create(
						&pcp_data,
						sizeof(PlayerConnectedPacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_host_broadcast(server, 0, player_connected_packet);
				}
				break;

				case ENET_EVENT_TYPE_RECEIVE:
				{
					HandleReceive(
						event.peer->incomingPeerID,
						event.packet
					);
				}
				break;

				case ENET_EVENT_TYPE_DISCONNECT:
				case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
				{
					std::cout
					<< "Client "
					<< event.peer->incomingPeerID
					<< "disconnected"
					<< std::endl;

					player_states.erase(event.peer->incomingPeerID);

					PlayerDisconnectedPacketData pdp_data;
					pdp_data.disconnected_player_id = event.peer->incomingPeerID;
					ENetPacket* player_disconnected_packet = enet_packet_create(
						&pdp_data,
						sizeof(PlayerDisconnectedPacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_host_broadcast(server, 0, player_disconnected_packet);
				}
				break;

				case ENET_EVENT_TYPE_NONE: break;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return 0;
}
