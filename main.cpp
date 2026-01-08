#include <vector>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <string>
#include <fstream>
#include <experimental/scope>
#include <thread>

#define SJ_IMPL
#include "libs/sj.h"

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

	// Server -> Client control packets
	CONTROL_MAP_DATA,
	CONTROL_GAME_START,
	CONTROL_GAME_END
};

#pragma region PACKETS

// Server <-> Clients
#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_CONNECTED;
	enet_uint8 connected_player_id;
} PlayerConnectedPacketData;

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
Vec3 hider_spawn = {};
Vec3 seeker_spawn = {};

bool game_started = false;

enet_uint8 current_seeker_id_index = 0;
std::chrono::time_point<std::chrono::steady_clock> current_seeker_timer;


static inline void HandleReceive(
	ENetPeer* peer,
	ENetPacket* packet
) {
	switch (*((PacketType*)(packet->data + 0))) {
		case PacketType::PLAYER_SYNC:
		{
			if (
				std::find(
					player_ids.begin(),
					player_ids.end(),
					peer->incomingPeerID
				) == player_ids.end()
			) {
				player_ids.push_back(peer->incomingPeerID);

				serverside_player_data[peer->incomingPeerID] = ServerPlayerData{};

				std::cout
				<< "Player "
				<< peer->incomingPeerID
				<< "connected"
				<< std::endl;

				PlayerConnectedPacketData pcp_data;
				pcp_data.connected_player_id = peer->incomingPeerID;
				ENetPacket* player_connected_packet = enet_packet_create(
					&pcp_data,
					sizeof(PlayerConnectedPacketData),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_host_broadcast(server, 0, player_connected_packet);

				char* mdp_data = new char[sizeof(ControlMapDataPacketHeader) + map_data.size()];
				ControlMapDataPacketHeader mdp_header;
				mdp_header.map_data_size = map_data.size();
				memcpy(mdp_data, &mdp_header, sizeof(ControlMapDataPacketHeader));
				memcpy(mdp_data+sizeof(ControlMapDataPacketHeader), map_data.c_str(), map_data.size());
				ENetPacket* map_data_packet = enet_packet_create(
					&mdp_data,
					sizeof(ControlMapDataPacketHeader) + map_data.size(),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_peer_send(peer, 0, map_data_packet);
				delete[] mdp_data;
			}

			player_states[peer->incomingPeerID] = *((PlayerState*)(
				packet->data + offsetof(PlayerSyncPacketData, player_state)
			));

			enet_host_broadcast(server, 0, packet);
		}
		break;

		case PacketType::PLAYER_READY:
		{
			serverside_player_data[peer->incomingPeerID].ready = true;

			int ready_players = 0;
			for (auto const& [_, ss_player_data] : serverside_player_data) {
				if (ss_player_data.ready) ready_players++;
			}
			if (ready_players != player_ids.size()) return;

			// Everyone is ready; start game

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
				player_states[peer->incomingPeerID].player_state_flags
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

			// Handle stats

			if (!player_stats.contains(peer->incomingPeerID)) player_stats[peer->incomingPeerID] = PlayerStats{};
			player_stats[peer->incomingPeerID].seek_time = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - current_seeker_timer
			).count();

			current_seeker_timer = std::chrono::steady_clock::now();

			if (!player_stats.contains(caught_hider_id)) player_stats[caught_hider_id] = PlayerStats{};
			player_stats[caught_hider_id].last_alive_rounds++;

			// End game if everyone has been a seeker
			if (current_seeker_id_index == player_ids.size()-1) {
				for (auto& [_, player_stat] : player_stats) {
					player_stat.points = (
						(player_stat.seek_time - player_stats.size()-1)
						+ player_stat.last_alive_rounds
					);

					PlayerStatsPacketData psp_data;
					psp_data.player_id = peer->incomingPeerID;
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

			// Advance round
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
	map_data.erase(std::remove_if(map_data.begin(), map_data.end(), []
	(unsigned char c){
		if (
			c == ' ' || c == '\n' || c == '\t'
		) return true;
	}), map_data.end());

	// Map parsing & validation
	sj_Reader r = sj_reader((char*)map_data.c_str(), map_data.size());
	sj_Value map_objects = sj_read(&r);
	if (map_objects.type != SJ_ARRAY) throw std::runtime_error(
		std::string("Provided map ") + map_path + " is invalid"
	);
	bool map_has_errors = false;
	std::string map_errors("");
	bool hider_spawn_found = false;
	int hider_spawn_mapobj_index = 0;
	bool seeker_spawn_found = false;
	int seeker_spawn_mapobj_index = 0;
	sj_Value map_object;
	while (sj_iter_array(&r, map_objects, &map_object)) {
		if (map_object.type != SJ_OBJECT) {
			int ln, col;
			sj_location(&r, &ln, &col);
			map_errors += std::string("Non-object found in base array at ")
				+ "line " + std::to_string(ln) + ','
				+ "col " + std::to_string(col) + '\n'
			;
			map_has_errors = true;
			continue;
		}

		sj_Value key, val;
		while (sj_iter_object(&r, map_object, &key, &val)) {
			if (
				(key.end - key.start) == sizeof("type")-1 &&
				memcmp(key.start, "type", sizeof("type")-1) == 0
			) {
				if (
					(val.end - val.start) >= sizeof("Hider_Spawn")-1 &&
					memcmp(val.start, "Hider_Spawn", sizeof("Hider_Spawn")-1) == 0
				) hider_spawn_found = true;
				else if (
					(val.end - val.start) >= sizeof("Seeker_Spawn")-1 &&
					memcmp(val.start, "Seeker_Spawn", sizeof("Seeker_Spawn")-1) == 0
				) seeker_spawn_found = true;
			}

			if (hider_spawn_found && seeker_spawn_found) break;
		}

		if (!hider_spawn_found) hider_spawn_mapobj_index++;
		if (!seeker_spawn_found) seeker_spawn_mapobj_index++;
	}
	if (!hider_spawn_found) {
		map_errors += "Hider_Spawn not found\n";
		map_has_errors = true;
	}
	if (!seeker_spawn_found) {
		map_errors += "Seeker_Spawn not found\n";
		map_has_errors = true;
	}
	if (!map_has_errors) {
		sj_Value key, val;

		r = sj_reader((char*)map_data.c_str(), map_data.size());
		map_objects = sj_read(&r);
		for (int i = 0; i < hider_spawn_mapobj_index; i++) {
			sj_iter_array(&r, map_objects, &map_object);
		}
		while (sj_iter_object(&r, map_object, &key, &val)) {
			// TODO: hider_spawn
		}

		r = sj_reader((char*)map_data.c_str(), map_data.size());
		map_objects = sj_read(&r);
		for (int i = 0; i < seeker_spawn_mapobj_index; i++) {
			sj_iter_array(&r, map_objects, &map_object);
		}
		while (sj_iter_object(&r, map_object, &key, &val)) {
			// TODO: seeker_spawn
		}
	}
	if (map_has_errors) throw std::runtime_error(
		std::string("Map ") + map_path + " has errors:\n"
		+ map_errors
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
						event.peer,
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
