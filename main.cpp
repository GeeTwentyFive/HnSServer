#include <vector>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <string>
#include <fstream>
#include <experimental/scope>
#include <thread>

#define ENET_IMPLEMENTATION
#include "libs/enet.h"

#ifdef _WIN32
#include <windows.h>
#endif


#define DEFAULT_PORT 55555
#define MAX_PLAYERS 8


#pragma pack(1)
typedef struct {
	float x = 0.0;
	float y = 0.0;
	float z = 0.0;
} Vec3;

enum PlayerStateFlags {
	ALIVE = 0,
	IS_SEEKER = 1,
	JUMPED = 2,
	WALLJUMPED = 4,
	SLIDING = 8,
	FLASHLIGHT = 16
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
	float seek_time = -1.0;
	char last_alive_rounds = 0;
	unsigned char points = 0;
} PlayerStats;


enum PacketType : char {
	PLAYER_CONNECTED,
	PLAYER_SYNC,
	PLAYER_READY,
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
	PacketType packet_type = PacketType::PLAYER_READY;
	enet_uint8 player_id;
} PlayerReadyPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_HIDER_CAUGHT;
	enet_uint8 player_id;
	enet_uint8 caught_hider_id;
} PlayerHiderCaughtPacketData;

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


ENetHost* server;

std::vector<enet_uint8> player_ids = []{
	std::vector<enet_uint8> v;
	v.reserve(MAX_PLAYERS);
	return v;
}();
std::unordered_map<enet_uint8, PlayerState> player_states;
std::unordered_map<enet_uint8, ServerPlayerData> serverside_player_data;
std::unordered_map<enet_uint8, PlayerStats> player_stats;

std::string map_data;
Vec3 hider_spawn = {}; // TODO
Vec3 seeker_spawn = {}; // TODO

bool game_started = false;

enet_uint8 current_seeker_id_index = 0;
std::chrono::time_point<std::chrono::steady_clock> current_seeker_timer;


static inline void HandleReceive(
	enet_uint8 player_id,
	ENetPacket* packet
) {
	PacketType packet_type = *((PacketType*)(packet->data + 0));

	if (
		packet_type != PacketType::PLAYER_SYNC &&
		packet_type != PacketType::PLAYER_READY &&
		packet_type != PacketType::PLAYER_HIDER_CAUGHT
		||
		player_id != *((enet_uint8*)(
			packet->data + offsetof(PlayerSyncPacketData, player_id)
		))
	) return;

	switch (packet_type) {
		case PacketType::PLAYER_SYNC:
		{
			if (
				std::find(
					player_ids.begin(),
					player_ids.end(),
					player_id
				) == player_ids.end()
			) {
				player_ids.push_back(player_id);

				serverside_player_data[player_id] = ServerPlayerData{};

				std::cout
				<< "Player "
				<< player_id
				<< "connected"
				<< std::endl;

				PlayerConnectedPacketData pcp_data;
				pcp_data.connected_player_id = player_id;
				ENetPacket* player_connected_packet = enet_packet_create(
					&pcp_data,
					sizeof(PlayerConnectedPacketData),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_host_broadcast(server, 0, player_connected_packet);

				// TODO: Send map_data
			}

			player_states[player_id] = *((PlayerState*)(
				packet->data + offsetof(PlayerSyncPacketData, player_state)
			));

			enet_host_broadcast(server, 0, packet);
		}
		break;

		case PacketType::PLAYER_READY:
		{
			serverside_player_data[player_id].ready = true;

			int ready_players = 0;
			for (auto const& [_, ss_player_state] : serverside_player_data) {
				if (ss_player_state.ready) ready_players++;
			}
			if (ready_players != player_ids.size()) return;

			current_seeker_timer = std::chrono::steady_clock::now();
			game_started = true;

			ControlGameStartPacketData cgsp_data;
			ENetPacket* game_start_packet = enet_packet_create(
				&cgsp_data,
				sizeof(ControlGameStartPacketData),
				ENET_PACKET_FLAG_RELIABLE
			);
			enet_host_broadcast(server, 0, game_start_packet);

			enet_packet_destroy(packet);
		}
		break;

		case PacketType::PLAYER_HIDER_CAUGHT:
		{
			if (!(
				player_states[player_id].player_state_flags
				& PlayerStateFlags::IS_SEEKER
			)) return;

			enet_uint8 caught_hider_id = *((enet_uint8*)(
				packet->data + offsetof(PlayerHiderCaughtPacketData, caught_hider_id)
			));

			player_states[caught_hider_id].player_state_flags &= ~(1 << PlayerStateFlags::ALIVE);

			int alive_hiders_left = 0;
			for (auto const& [_, player_state] : player_states) {
				if (player_state.player_state_flags & PlayerStateFlags::IS_SEEKER) continue;
				if (player_state.player_state_flags & PlayerStateFlags::ALIVE) alive_hiders_left++;
			}
			if (alive_hiders_left != 0) return;

			if (!player_stats.contains(player_id)) player_stats[player_id] = PlayerStats{};
			player_stats[player_id].seek_time = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - current_seeker_timer
			).count();

			current_seeker_timer = std::chrono::steady_clock::now();

			if (!player_stats.contains(caught_hider_id)) player_stats[caught_hider_id] = PlayerStats{};
			player_stats[caught_hider_id].last_alive_rounds++;

			if (current_seeker_id_index == player_ids.size()-1) {
				for (auto& [_, player_stat] : player_stats) {
					player_stat.points = (
						(player_stat.seek_time - player_stats.size()-1)
						+ player_stat.last_alive_rounds
					);

					PlayerStatsPacketData psp_data;
					psp_data.player_id = player_id;
					psp_data.player_stats = player_stat;
					ENetPacket* player_stats_packet = enet_packet_create(
						&psp_data,
						sizeof(PlayerStatsPacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_host_broadcast(server, 0, player_stats_packet);
				}

				ControlGameEndPacketData cgp_data;
				ENetPacket* control_game_end_packet = enet_packet_create(
					&cgp_data,
					sizeof(ControlGameEndPacketData),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_host_broadcast(server, 0, control_game_end_packet);

				enet_host_flush(server);

				exit(0);
			}

			current_seeker_id_index++;

			for (auto& [player_id, player_state] : player_states) {
				if (player_id == player_ids[current_seeker_id_index]) {
					player_state.player_state_flags |= (1 << PlayerStateFlags::IS_SEEKER);
					player_state.position = seeker_spawn;
				}
				else {
					player_state.player_state_flags &= ~(1 << PlayerStateFlags::IS_SEEKER);
					player_state.position = hider_spawn;
				}

				player_state.player_state_flags |= (1 << PlayerStateFlags::ALIVE);

				PlayerSyncPacketData psp_data;
				psp_data.player_id = player_id;
				psp_data.player_state = player_state;
				ENetPacket* player_state_packet = enet_packet_create(
					&psp_data,
					sizeof(PlayerSyncPacketData),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_host_broadcast(server, 0, player_state_packet);
			}

			enet_packet_destroy(packet);
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

	std::ifstream map_file_stream(map_path);
	map_file_stream.seekg(0, std::ios::end);
	map_data.reserve(map_file_stream.tellg());
	map_file_stream.seekg(0, std::ios::beg);
	map_data.assign(
		std::istreambuf_iterator<char>(map_file_stream),
		std::istreambuf_iterator<char>()
	);

	if (!enet_initialize()) throw std::runtime_error("Failed to initialize ENet");
	auto _cleanup0 = std::experimental::scope_exit(enet_deinitialize);

	ENetAddress address = {0};
	address.host = ENET_HOST_ANY;
	address.port = port;
	server = enet_host_create(&address, MAX_PLAYERS, 1, 0, 0);
	if (server == nullptr) throw std::runtime_error("Failed to create ENet server");
	auto _cleanup1 = std::experimental::scope_exit([]{enet_host_destroy(server);});

	std::cout << "Server started on port " << port << std::endl;

	#ifdef _WIN32
	timeBeginPeriod(1);
	auto _cleanup_windows = std::experimental::scope_exit([]{timeEndPeriod(1);});
	#endif

	ENetEvent event;
	for (;;) {
		while (enet_host_service(server, &event, 0) > 0) {
			switch (event.type) {
				case ENET_EVENT_TYPE_CONNECT:
				{
					if (game_started) {
						enet_peer_disconnect(event.peer, 0);
						enet_host_flush(server);
						enet_peer_reset(event.peer);
						continue;
					}
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
					if (
						std::find(
							player_ids.begin(),
							player_ids.end(),
							event.peer->incomingPeerID
						) == player_ids.end()
					) continue;

					std::cout
					<< "Player "
					<< event.peer->incomingPeerID
					<< "disconnected"
					<< std::endl;

					player_states.erase(event.peer->incomingPeerID);
					player_ids.erase(
						std::remove(
							player_ids.begin(),
							player_ids.end(),
							event.peer->incomingPeerID
						),
						player_ids.end()
					);

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
