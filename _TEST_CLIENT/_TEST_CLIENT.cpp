#include <iostream>
#include <thread>

#define ENET_IMPLEMENTATION
#include "../libs/enet.h"


#define PORT 55555
#define NAME "TEST CLIENT"

#define CONNECT_TIMEOUT_MS 1000

#define MAX_NAME_LENGTH 64


#pragma pack(1)
typedef struct {
	float x = 0.0;
	float y = 0.0;
	float z = 0.0;
} Vec3;

enum PlayerStateFlags {
	ALIVE = 1 << 0,
	IS_SEEKER = 1 << 1,
	JUMPED = 1 << 2,
	WALLJUMPED = 1 << 3,
	SLIDING = 1 << 4,
	FLASHLIGHT = 1 << 5
};

#pragma pack(1)
typedef struct {
	Vec3 position;
	float yaw;
	float pitch;
	char player_state_flags; // PlayerStateFlags bitmask
	Vec3 hook_point;
} PlayerState;

#pragma pack(1)
typedef struct {
	bool ready = false;
} ServerPlayerData;

#pragma pack(1)
typedef struct {
	char name[MAX_NAME_LENGTH] = {0};
	float seek_time = -1.0;
	char last_alive_rounds = 0;
	unsigned char points = 0;
} PlayerStats;


enum PacketType : char {
	PLAYER_SYNC,
	PLAYER_SET_NAME,
	PLAYER_READY,
	PLAYER_HIDER_CAUGHT,
	PLAYER_STATS,
	PLAYER_DISCONNECTED,

	// Server -> Client control packets
	CONTROL_MAP_DATA,
	CONTROL_GAME_START,
	CONTROL_GAME_END
};

#pragma region PACKETS

// Server <-> Clients
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_SYNC;
	enet_uint8 player_id;
	PlayerState player_state;
} PlayerSyncPacketData;

// Client -> Server
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_SET_NAME;
	char name[MAX_NAME_LENGTH];
} PlayerSetNamePacketData;

// Client -> Server
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_READY;
} PlayerReadyPacketData;

// Client (seeker) -> Server
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_HIDER_CAUGHT;
	enet_uint8 caught_hider_id;
} PlayerHiderCaughtPacketData;

// Server -> Clients
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_STATS;
	enet_uint8 player_id;
	PlayerStats player_stats;
} PlayerStatsPacketData;

// Server <-> Clients
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_DISCONNECTED;
	enet_uint8 disconnected_player_id;
} PlayerDisconnectedPacketData;


// Server -> Clients control packets

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_MAP_DATA;
} ControlMapDataPacketHeader;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_GAME_START;
} ControlGameStartPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_GAME_END;
} ControlGameEndPacketData;

#pragma endregion PACKETS


PlayerState local_state = {
	.position = {1.1, 2.2, 3.01},
	.yaw = 3.14,
	.pitch = 7.27,
	.player_state_flags = (char)0b101010,
	.hook_point = {0.01, 0.02, 0.03}
};

int main() {
	if (enet_initialize() != 0) {
		std::cout << "Failed to initialize ENet" << std::endl;
		exit(1);
	}
	atexit(enet_deinitialize);

	ENetHost* client = enet_host_create(NULL, 1, 1, 0, 0);
	if (!client) {
		std::cout << "Failed to create ENet client" << std::endl;
		exit(1);
	}

	ENetAddress address = {0};
	enet_address_set_host(&address, "::1");
	address.port = PORT;
	ENetPeer* server_peer = enet_host_connect(client, &address, 1, 0);
	if (!server_peer) {
		std::cout << "Failed to create ENet peer" << std::endl;
		exit(1);
	}

	ENetEvent event;

	if (
		enet_host_service(client, &event, CONNECT_TIMEOUT_MS) > 0 &&
		event.type == ENET_EVENT_TYPE_CONNECT
	) std::cout << "Connected to server" << std::endl;
	else {
		std::cout << "Failed to connect to server" << std::endl;
		enet_peer_reset(server_peer);
		exit(1);
	}

	PlayerSyncPacketData psp_data{};
	psp_data.player_state = local_state;
	ENetPacket* initial_sync_packet = enet_packet_create(
		&psp_data,
		sizeof(PlayerSyncPacketData),
		ENET_PACKET_FLAG_RELIABLE
	);
	enet_peer_send(server_peer, 0, initial_sync_packet);

	PlayerSetNamePacketData psn_data{};
	memcpy(psn_data.name, NAME, sizeof(NAME));
	ENetPacket* set_name_packet = enet_packet_create(
		&psn_data,
		sizeof(PlayerSetNamePacketData),
		ENET_PACKET_FLAG_RELIABLE
	);
	enet_peer_send(server_peer, 0, set_name_packet);

	PlayerReadyPacketData prp_data{};
	ENetPacket* ready_packet = enet_packet_create(
		&prp_data,
		sizeof(PlayerReadyPacketData),
		ENET_PACKET_FLAG_RELIABLE
	);
	enet_peer_send(server_peer, 0, ready_packet);

	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		PlayerSyncPacketData psp_data{};
		psp_data.player_state = local_state;
		ENetPacket* sync_packet = enet_packet_create(
			&psp_data,
			sizeof(PlayerSyncPacketData),
			0
		);
		enet_peer_send(server_peer, 0, sync_packet);

		while (enet_host_service(client, &event, 0) > 0) {
			if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;

			switch (*((PacketType*)(event.packet->data + 0))) {
				default: break;

				case PacketType::PLAYER_SYNC:
				{
					std::cout
					<< "Received player "
					<< +*(
						event.packet->data +
						offsetof(PlayerSyncPacketData, player_id)
					)
					<< " state:"
					<< std::endl;

					PlayerState received_state = *((PlayerState*)(
						event.packet->data +
						offsetof(PlayerSyncPacketData, player_state)
					));

					std::cout << "position x: " << received_state.position.x << std::endl;
					std::cout << "position y: " << received_state.position.y << std::endl;
					std::cout << "position z: " << received_state.position.z << std::endl;
					std::cout << "yaw: " << received_state.yaw << std::endl;
					std::cout << "pitch: " << received_state.pitch << std::endl;
					std::cout << "state flags: " << std::endl;
					std::cout << "\tALIVE: " << ((received_state.player_state_flags & PlayerStateFlags::ALIVE) != 0) << std::endl;
					std::cout << "\tIS_SEEKER: " << ((received_state.player_state_flags & PlayerStateFlags::IS_SEEKER) != 0) << std::endl;
					std::cout << "\tJUMPED: " << ((received_state.player_state_flags & PlayerStateFlags::JUMPED) != 0) << std::endl;
					std::cout << "\tWALLJUMPED: " << ((received_state.player_state_flags & PlayerStateFlags::WALLJUMPED) != 0) << std::endl;
					std::cout << "\tSLIDING: " << ((received_state.player_state_flags & PlayerStateFlags::SLIDING) != 0) << std::endl;
					std::cout << "\tFLASHLIGHT: " << ((received_state.player_state_flags & PlayerStateFlags::FLASHLIGHT) != 0) << std::endl;
					std::cout << "hook_point x: " << received_state.hook_point.x << std::endl;
					std::cout << "hook_point y: " << received_state.hook_point.y << std::endl;
					std::cout << "hook_point z: " << received_state.hook_point.z << std::endl;
				}
				break;

				case PacketType::PLAYER_STATS:
				{
					std::cout
					<< "Received player "
					<< +*(
						event.packet->data +
						offsetof(PlayerStatsPacketData, player_id)
					)
					<< " stats:"
					<< std::endl;

					PlayerStats received_stats = *((PlayerStats*)(
						event.packet->data +
						offsetof(PlayerStatsPacketData, player_stats)
					));

					std::cout << "name: " << received_stats.name << std::endl;
					std::cout << "seek_time: " << received_stats.seek_time << std::endl;
					std::cout << "last_alive_rounds: " << +received_stats.last_alive_rounds << std::endl;
					std::cout << "points: " << +received_stats.points << std::endl;
				}
				break;

				case PacketType::PLAYER_DISCONNECTED:
				{
					std::cout
					<< "Player "
					<< +*(
						event.packet->data +
						offsetof(PlayerDisconnectedPacketData, disconnected_player_id)
					)
					<< " disconnected"
					<< std::endl;
				}
				break;


				case PacketType::CONTROL_MAP_DATA:
				{
					std::cout << "Map data received:" << std::endl;
					std::cout
					<< std::string(
						(char*)(event.packet->data + sizeof(ControlMapDataPacketHeader)),
						event.packet->dataLength - sizeof(ControlMapDataPacketHeader)
					)
					<< std::endl;
				}
				break;

				case PacketType::CONTROL_GAME_START:
				{
					std::cout << "Game start received" << std::endl;
				}
				break;

				case PacketType::CONTROL_GAME_END:
				{
					std::cout << "Game end received" << std::endl;
					exit(0);
				}
				break;
			}

			enet_packet_destroy(event.packet);
		}

		PlayerHiderCaughtPacketData phpc_data{};
		phpc_data.caught_hider_id = 0;
		ENetPacket* hider_caught_packet = enet_packet_create(
			&phpc_data,
			sizeof(PlayerHiderCaughtPacketData),
			0
		);
		enet_peer_send(server_peer, 0, hider_caught_packet);
	}

	return 0;
}