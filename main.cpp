#include <limits>
#include <unordered_map>
#include <chrono>
#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <array>

#include "libs/json.hpp"
#define ENET_IMPLEMENTATION
#include "libs/enet.h"

#ifdef _WIN32
#include <windows.h>
#undef max
#endif


#define DEFAULT_PORT 55555
#define MAX_PLAYERS 8

#define MAX_NAME_LENGTH 64


typedef uint16_t PlayerID;

PlayerID _player_GUID = 0;
static inline const PlayerID NewPlayerGUID() {
	if (_player_GUID == std::numeric_limits<PlayerID>::max()) throw std::runtime_error(
		"Player GUID counter overflow"
	);

	return _player_GUID++;
}


#pragma pack(1)
typedef struct {
	float x = 0.0;
	float y = 0.0;
	float z = 0.0;
} Vec3;


enum PlayerStateFlags : uint8_t {
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
	uint8_t player_state_flags; // PlayerStateFlags bitmask
	Vec3 hook_point;
} PlayerState;

typedef struct {
	bool ready = false;
        bool was_seeker = false;
} ServerPlayerData;

#pragma pack(1)
typedef struct {
	char name[MAX_NAME_LENGTH] = {0};
	float seek_time = -1.0;
	char last_alive_rounds = 0;
	unsigned char points = 0;
} PlayerStats;


enum PacketType : char {
	PLAYER_SYNC, // Client -> Server -> other Clients
	PLAYER_SET_NAME, // Client -> Server
	PLAYER_READY, // Client -> Server
	PLAYER_HIDER_CAUGHT, // Client (seeker) -> Server
	PLAYER_STATS, // Server -> Clients
	PLAYER_DISCONNECTED, // Server -> Clients

	// Server -> Client control packets
	CONTROL_MAP_DATA,
	CONTROL_GAME_START,
	CONTROL_SET_PLAYER_STATE,
	CONTROL_GAME_END
};

#pragma region PACKETS_DATA

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_SYNC;
	PlayerID player_id;
	PlayerState player_state;
} PlayerSyncPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_SET_NAME;
	char name[MAX_NAME_LENGTH];
} PlayerSetNamePacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_HIDER_CAUGHT;
	PlayerID caught_hider_id;
} PlayerHiderCaughtPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_STATS;
	PlayerID player_id;
	PlayerStats player_stats;
} PlayerStatsPacketData;

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::PLAYER_DISCONNECTED;
	PlayerID disconnected_player_id;
} PlayerDisconnectedPacketData;


// Server -> Clients control packets

#pragma pack(1)
typedef struct {
	PacketType packet_type = PacketType::CONTROL_SET_PLAYER_STATE;
	PlayerState state;
} ControlSetPlayerStatePacketData;

#pragma endregion PACKETS_DATA


ENetHost* server;

std::unordered_map<ENetPeer*, PlayerID> peer_to_player_id(MAX_PLAYERS);
std::unordered_map<PlayerID, ENetPeer*> player_id_to_peer(MAX_PLAYERS);

std::unordered_map<PlayerID, PlayerState> player_states(MAX_PLAYERS);
std::unordered_map<PlayerID, ServerPlayerData> serverside_player_data(MAX_PLAYERS);
std::unordered_map<PlayerID, PlayerStats> players_stats(MAX_PLAYERS);

std::string map_data;
Vec3 hider_spawn = {};
Vec3 seeker_spawn = {};

bool game_started = false;

PlayerID current_seeker_id;
std::chrono::time_point<std::chrono::steady_clock> current_seeker_timer;

#ifdef _HNS_DEBUG
std::ofstream _DEBUG_LOG("HnSServer.log");
#endif // _HNS_DEBUG


static inline void HandleHiderCaughtPacket(
	ENetPeer* peer,
	ENetPacket* packet
) {
	if (packet->dataLength < sizeof(PlayerHiderCaughtPacketData)) {
		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Received packet PLAYER_HIDER_CAUGHT size " << packet->dataLength
			<< " is less than size of PlayerHiderCaughtPacketData " << sizeof(PlayerHiderCaughtPacketData)
			<< std::endl;
		#endif // _HNS_DEBUG

		return;
	}
	if (peer_to_player_id.find(peer) == peer_to_player_id.end()) return;


	const PlayerID player_id = peer_to_player_id[peer];


	// If not sent by seeker: return
	if (!(
		player_states[player_id].player_state_flags
		& PlayerStateFlags::IS_SEEKER
	)) {
		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Received packet PLAYER_HIDER_CAUGHT sender "
			<< player_id
			<< " is found to not be a seeker; dropping"
			<< std::endl;
		#endif // _HNS_DEBUG

		return;
	}


	PlayerID caught_hider_id = *((PlayerID*)(
		packet->data + offsetof(PlayerHiderCaughtPacketData, caught_hider_id)
	));


	#ifdef _HNS_DEBUG
		_DEBUG_LOG
		<< "Received packet PLAYER_HIDER_CAUGHT from Player "
		<< player_id
		<< ", with caught hider ID "
		<< caught_hider_id
		<< std::endl;
	#endif // _HNS_DEBUG


	// Kill caught hider
	player_states[caught_hider_id].player_state_flags &= ~PlayerStateFlags::ALIVE;
	{
	ControlSetPlayerStatePacketData cspsp_data{};
	cspsp_data.state = player_states[caught_hider_id];
	ENetPacket* set_state_packet = enet_packet_create(
		&cspsp_data,
		sizeof(ControlSetPlayerStatePacketData),
		ENET_PACKET_FLAG_RELIABLE
	);
	enet_peer_send(player_id_to_peer[caught_hider_id], 0, set_state_packet);

	#ifdef _HNS_DEBUG
		_DEBUG_LOG
		<< "Sending packet CONTROL_SET_PLAYER_STATE to "
		<< caught_hider_id
		<< " with data:"
		<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
		<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
		<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
		<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
		<< "\n- yaw: " << cspsp_data.state.yaw
		<< "\n- pitch: " << cspsp_data.state.pitch
		<< "\n- flags:"
		<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
		<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
		<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
		<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
		<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
		<< std::endl;
	#endif // _HNS_DEBUG
	}


	// If alive hiders still left in round: return
	int alive_hiders_left = 0;
	for (auto const& [_, player_state] : player_states) {
		if (player_state.player_state_flags & PlayerStateFlags::IS_SEEKER) continue;
		if (player_state.player_state_flags & PlayerStateFlags::ALIVE) alive_hiders_left++;
	}
	#ifdef _HNS_DEBUG
		_DEBUG_LOG << "alive_hiders_left == " << alive_hiders_left << std::endl;
	#endif // _HNS_DEBUG
	if (alive_hiders_left != 0) return;


	// Set/calculate players stats

	players_stats[player_id].seek_time = std::chrono::duration<float>(
		std::chrono::steady_clock::now() - current_seeker_timer
	).count();

	current_seeker_timer = std::chrono::steady_clock::now();

	serverside_player_data[player_id].was_seeker = true;

	players_stats[caught_hider_id].last_alive_rounds++;

	#ifdef _HNS_DEBUG
		_DEBUG_LOG
		<< "Set/calculated post-round player stats:"
		<< "\n- seeker " << player_id << " seek time: "
			<<  players_stats[player_id].seek_time
		<< "\n- hider " << caught_hider_id << " last alive rounds: "
			<< std::to_string(players_stats[caught_hider_id].last_alive_rounds)
		<< std::endl;
	#endif // _HNS_DEBUG


	// End game if everyone has been a seeker
	int done_seekers_count = 0;
	for (auto const& [_, ss_player_data] : serverside_player_data) {
		if (ss_player_data.was_seeker) done_seekers_count++;
	}
	#ifdef _HNS_DEBUG
		_DEBUG_LOG
		<< "done_seekers_count == " << done_seekers_count
		<< std::endl;
	#endif // _HNS_DEBUG
	if (done_seekers_count == peer_to_player_id.size()) {
		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Everyone was a seeker; ending game..."
			<< std::endl;
		#endif // _HNS_DEBUG

		std::vector<std::pair<PlayerID, float>> seek_times_sorted;
		seek_times_sorted.reserve(players_stats.size());
		for (auto const& [player_id, player_stats] : players_stats) {
			seek_times_sorted.push_back({
				player_id,
				player_stats.seek_time
			});
		}
		std::sort(
			seek_times_sorted.begin(),
			seek_times_sorted.end(),
			[](
				const std::pair<PlayerID, float>& st1,
				const std::pair<PlayerID, float>& st2
			){
				return st1.second < st2.second;
			}
		);
		std::unordered_map<PlayerID, int> seek_time_placements;
		seek_time_placements.reserve(players_stats.size());
		for (int i = 0; i < players_stats.size(); i++) {
			seek_time_placements[seek_times_sorted[i].first] = i;
		}
		for (auto& [player_id, player_stats] : players_stats) {
			// points = (player_count - seek_placement - 1) + last_alive_rounds
			player_stats.points = (
				(players_stats.size() - seek_time_placements[player_id] - 1)
				+ player_stats.last_alive_rounds
			);

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Final player " << player_id << " stats:"
				<< "\n- name: " << player_stats.name
				<< "\n- seek time: " << player_stats.seek_time
				<< "\n- seek time placement: " << seek_time_placements[player_id]
				<< "\n- last alive rounds: " << std::to_string(player_stats.last_alive_rounds)
				<< "\n- points: " << std::to_string(player_stats.points)
				<< std::endl;
			#endif // _HNS_DEBUG

			PlayerStatsPacketData psp_data{};
			psp_data.player_id = player_id;
			psp_data.player_stats = player_stats;
			ENetPacket* player_stats_packet = enet_packet_create(
				&psp_data,
				sizeof(PlayerStatsPacketData),
				ENET_PACKET_FLAG_RELIABLE
			);
			enet_host_broadcast(server, 0, player_stats_packet);

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Broadcasting aforementioned stats"
				<< " with packet PLAYER_STATS " << std::to_string(PacketType::PLAYER_STATS)
				<< std::endl;
			#endif // _HNS_DEBUG
		}

		ENetPacket* control_game_end_packet = enet_packet_create(
			std::array<char, 1>{PacketType::CONTROL_GAME_END}.data(),
			sizeof(PacketType),
			ENET_PACKET_FLAG_RELIABLE
		);
		enet_host_broadcast(server, 0, control_game_end_packet);

		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Broadcasting packet CONTROL_GAME_END "
			<< PacketType::CONTROL_GAME_END
			<< std::endl;
		#endif // _HNS_DEBUG

		enet_host_flush(server);

		std::cout << "Game ended, shutting down..." << std::endl;

		exit(0);
	}


	// Advance round (set players states -> respawn)

	#ifdef _HNS_DEBUG
		_DEBUG_LOG << "Advancing to next round..." << std::endl;
	#endif // _HNS_DEBUG

	serverside_player_data[player_id].was_seeker = true;

	PlayerID next_seeker_id = -1;
	for (auto const& [player_id, ss_player_data] : serverside_player_data) {
		if (ss_player_data.was_seeker == false) {
			next_seeker_id = player_id;
			break;
		}
	}
	#ifdef _HNS_DEBUG
		_DEBUG_LOG << "Next seeker ID: " << next_seeker_id << std::endl;
	#endif // _HNS_DEBUG
	{
	player_states[next_seeker_id].player_state_flags |= PlayerStateFlags::IS_SEEKER;
	player_states[next_seeker_id].player_state_flags |= PlayerStateFlags::ALIVE;
	player_states[next_seeker_id].position = seeker_spawn;
	ControlSetPlayerStatePacketData cspsp_data{};
	cspsp_data.state = player_states[next_seeker_id];
	ENetPacket* set_state_packet = enet_packet_create(
		&cspsp_data,
		sizeof(ControlSetPlayerStatePacketData),
		ENET_PACKET_FLAG_RELIABLE
	);
	enet_peer_send(player_id_to_peer[next_seeker_id], 0, set_state_packet);

	#ifdef _HNS_DEBUG
		_DEBUG_LOG
		<< "Sending packet CONTROL_SET_PLAYER_STATE to "
		<< next_seeker_id
		<< " with data:"
		<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
		<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
		<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
		<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
		<< "\n- yaw: " << cspsp_data.state.yaw
		<< "\n- pitch: " << cspsp_data.state.pitch
		<< "\n- flags:"
		<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
		<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
		<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
		<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
		<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
		<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
		<< std::endl;
	#endif // _HNS_DEBUG
	}
	current_seeker_id = next_seeker_id;

	for (auto& [_player_id, player_state] : player_states) {
		if (_player_id == next_seeker_id) continue;

		player_state.player_state_flags &= ~PlayerStateFlags::IS_SEEKER;
		player_state.player_state_flags |= PlayerStateFlags::ALIVE;
		player_state.position = hider_spawn;
		ControlSetPlayerStatePacketData cspsp_data{};
		cspsp_data.state = player_state;
		ENetPacket* set_state_packet = enet_packet_create(
			&cspsp_data,
			sizeof(ControlSetPlayerStatePacketData),
			ENET_PACKET_FLAG_RELIABLE
		);
		enet_peer_send(player_id_to_peer[_player_id], 0, set_state_packet);

		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Sending packet CONTROL_SET_PLAYER_STATE to "
			<< _player_id
			<< " with data:"
			<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
			<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
			<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
			<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
			<< "\n- yaw: " << cspsp_data.state.yaw
			<< "\n- pitch: " << cspsp_data.state.pitch
			<< "\n- flags:"
			<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
			<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
			<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
			<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
			<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
			<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
			<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
			<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
			<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
			<< std::endl;
		#endif // _HNS_DEBUG
	}
}


static inline void HandleReceive(
	ENetPeer* peer,
	ENetPacket* packet
) {
        if (packet->dataLength < sizeof(PacketType)) {
		#ifdef _HNS_DEBUG
			_DEBUG_LOG
			<< "Received packet data length " << packet->dataLength
			<< " is less than size of PacketType: " << sizeof(PacketType)
			<< std::endl;
		#endif // _HNS_DEBUG

		enet_packet_destroy(packet);
		return;
	}

        switch (*((PacketType*)(packet->data + 0))) {
                default: break;

		case PacketType::PLAYER_HIDER_CAUGHT:
                {
			HandleHiderCaughtPacket(peer, packet);
                }
                break;

                case PacketType::PLAYER_SYNC:
                {
                        if (packet->dataLength < sizeof(PlayerSyncPacketData)) {
				#ifdef _HNS_DEBUG
					_DEBUG_LOG
					<< "Received packet PLAYER_SYNC data size " << packet->dataLength
					<< " is less than size of PlayerSyncPacketData " << sizeof(PlayerSyncPacketData)
					<< std::endl;
				#endif // _HNS_DEBUG

				break;
			}

			if (peer_to_player_id.find(peer) == peer_to_player_id.end()) {
                                const PlayerID player_id = NewPlayerGUID();

                                peer_to_player_id[peer] = player_id;
                                player_id_to_peer[player_id] = peer;

                                serverside_player_data[player_id] = ServerPlayerData{};
                                players_stats[player_id] = PlayerStats{};

                                std::cout
				<< "Player "
				<< player_id
				<< " connected"
				<< std::endl;

                                std::vector<char> map_data_packet_data(sizeof(PacketType) + map_data.size());
                                map_data_packet_data[0] = PacketType::CONTROL_MAP_DATA;
                                memcpy(map_data_packet_data.data() + 1, map_data.c_str(), map_data.size());
                                ENetPacket* map_data_packet = enet_packet_create(
					map_data_packet_data.data(),
					sizeof(PacketType) + map_data.size(),
					ENET_PACKET_FLAG_RELIABLE
				);
				enet_peer_send(peer, 0, map_data_packet);

				#ifdef _HNS_DEBUG
					_DEBUG_LOG
					<< "Sending packet CONTROL_MAP_DATA to player "
					<< player_id
					<< std::endl;
				#endif // _HNS_DEBUG
                        }

                        const PlayerID player_id = peer_to_player_id[peer];

                        player_states[player_id] = *((PlayerState*)(
				packet->data + offsetof(PlayerSyncPacketData, player_state)
			));

			//PLAYER_SYNC
			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Received packet PLAYER_SYNC from player "
				<< player_id
				<< " with data:"
				<< "\n- pos X: " << player_states[player_id].position.x
				<< "\n- pos Y: " << player_states[player_id].position.y
				<< "\n- pos Z: " << player_states[player_id].position.z
				<< "\n- yaw: " << player_states[player_id].yaw
				<< "\n- pitch: " << player_states[player_id].pitch
				<< "\n- flags:"
				<< "\n^ - ALIVE: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::ALIVE) > 0)
				<< "\n^ - IS_SEEKER: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
				<< "\n^ - JUMPED: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - WALLJUMP: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - SLIDING: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - FLASHLIGHT: " << ((player_states[player_id].player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n- hook_point X: " << player_states[player_id].hook_point.x
				<< "\n- hook_point Y: " << player_states[player_id].hook_point.y
				<< "\n- hook_point Z: " << player_states[player_id].hook_point.z
				<< std::endl;
			#endif // _HNS_DEBUG

			if (player_states[player_id].position.y < 0.0) {
				#ifdef _HNS_DEBUG
					_DEBUG_LOG
					<< "Player "
					<< player_id
					<< "'s Y "
					<< player_states[player_id].position.y
					<< " is below 0.0"
					<< std::endl;
				#endif // _HNS_DEBUG

				if (
					player_id != current_seeker_id &&
					!(player_states[player_id].player_state_flags & PlayerStateFlags::IS_SEEKER) &&
					player_states[player_id].player_state_flags & PlayerStateFlags::ALIVE
				) {
					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Player below Y 0.0 is found to be a hider\n"
						<< "^ sending PLAYER_HIDER_CAUGHT packet to self as seeker..."
						<< std::endl;
					#endif // _HNS_DEBUG

					PlayerHiderCaughtPacketData phcp_data{};
					phcp_data.caught_hider_id = player_id;
					ENetPacket* hider_caught_packet = enet_packet_create(
						&phcp_data,
						sizeof(PlayerHiderCaughtPacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					HandleHiderCaughtPacket(
						player_id_to_peer[current_seeker_id],
						hider_caught_packet
					);
					enet_packet_destroy(hider_caught_packet);
				}
				else if (
					player_id == current_seeker_id ||
					player_states[player_id].player_state_flags & PlayerStateFlags::IS_SEEKER
				) {
					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Player below Y 0.0 is found to be a seeker"
						<< std::endl;
					#endif // _HNS_DEBUG

					player_states[player_id].position = seeker_spawn;
					ControlSetPlayerStatePacketData cspsp_data{};
					cspsp_data.state = player_states[player_id];
					ENetPacket* set_state_packet = enet_packet_create(
						&cspsp_data,
						sizeof(ControlSetPlayerStatePacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_peer_send(player_id_to_peer[player_id], 0, set_state_packet);

					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Sending packet CONTROL_SET_PLAYER_STATE to "
						<< player_id
						<< " with data:"
						<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
						<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
						<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
						<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
						<< "\n- yaw: " << cspsp_data.state.yaw
						<< "\n- pitch: " << cspsp_data.state.pitch
						<< "\n- flags:"
						<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
						<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
						<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
						<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
						<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
						<< std::endl;
					#endif // _HNS_DEBUG
				}
				else {
					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Player below Y 0.0 is found to be a spectator"
						<< std::endl;
					#endif // _HNS_DEBUG
	
					player_states[player_id].position = hider_spawn;
					ControlSetPlayerStatePacketData cspsp_data{};
					cspsp_data.state = player_states[player_id];
					ENetPacket* set_state_packet = enet_packet_create(
						&cspsp_data,
						sizeof(ControlSetPlayerStatePacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_peer_send(player_id_to_peer[player_id], 0, set_state_packet);

					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Sending packet CONTROL_SET_PLAYER_STATE to "
						<< player_id
						<< " with data:"
						<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
						<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
						<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
						<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
						<< "\n- yaw: " << cspsp_data.state.yaw
						<< "\n- pitch: " << cspsp_data.state.pitch
						<< "\n- flags:"
						<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
						<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
						<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
						<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
						<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
						<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
						<< std::endl;
					#endif // _HNS_DEBUG
				}
			}

                        ((PlayerSyncPacketData*)packet->data)->player_id = player_id;
			((PlayerSyncPacketData*)packet->data)->player_state = player_states[player_id];
                        // Retransmit sync packet to all other peers except the one who sent it
                        for (auto const& [player_peer, _] : peer_to_player_id) {
                                if (player_peer == peer) continue;
                                enet_peer_send(
                                        player_peer,
                                        0,
                                        enet_packet_create(
                                                packet->data,
                                                packet->dataLength,
                                                0
                                        )
                                );
                        }

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Retransmitting the (possibly server-modified) sync packet"
				<< " to all other peers except the one who sent it..."
				<< std::endl;
			#endif // _HNS_DEBUG
                }
                break;

                case PacketType::PLAYER_SET_NAME:
                {
			if (peer_to_player_id.find(peer) == peer_to_player_id.end()) break;
                        if (packet->dataLength < sizeof(PlayerSetNamePacketData)) {
				#ifdef _HNS_DEBUG
					_DEBUG_LOG
					<< "Received packet PLAYER_SET_NAME size " << packet->dataLength
					<< " is less than size of PlayerSetNamePacketData " << sizeof(PlayerSetNamePacketData)
					<< std::endl;
				#endif // _HNS_DEBUG

				break;
			}

                        memcpy(
                                players_stats[peer_to_player_id[peer]].name,
                                packet->data + offsetof(PlayerSetNamePacketData, name),
                                MAX_NAME_LENGTH
                        );

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Received packet PLAYER_SET_NAME with data: "
				<< players_stats[peer_to_player_id[peer]].name
				<< std::endl;
			#endif // _HNS_DEBUG
                }
                break;

                case PacketType::PLAYER_READY:
                {
			if (game_started) break;
			if (peer_to_player_id.find(peer) == peer_to_player_id.end()) break;
			if (serverside_player_data[peer_to_player_id[peer]].ready) break;

                        const PlayerID player_id = peer_to_player_id[peer];

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Received packet PLAYER_READY from player "
				<< player_id
				<< std::endl;
			#endif // _HNS_DEBUG

                        serverside_player_data[player_id].ready = true;

                        int ready_players = 0;
			for (auto const& [_, ss_player_data] : serverside_player_data) {
				if (ss_player_data.ready) ready_players++;
			}
			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "ready_players == "
				<< ready_players
				<< std::endl;
			#endif // _HNS_DEBUG
			if (ready_players != peer_to_player_id.size()) break;

                        // Everyone is ready; start game

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Everyone is ready; starting game..."
				<< std::endl;
			#endif // _HNS_DEBUG

			current_seeker_id = peer_to_player_id.begin()->second;

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "First chosen seeker id: "
				<< current_seeker_id
				<< std::endl;
			#endif // _HNS_DEBUG

			{
                        player_states[current_seeker_id].player_state_flags |= PlayerStateFlags::IS_SEEKER;
                        player_states[current_seeker_id].player_state_flags |= PlayerStateFlags::ALIVE;
                        player_states[current_seeker_id].position = seeker_spawn;
                        ControlSetPlayerStatePacketData cspsp_data{};
                        cspsp_data.state = player_states[current_seeker_id];
                        ENetPacket* set_state_packet = enet_packet_create(
                                &cspsp_data,
                                sizeof(ControlSetPlayerStatePacketData),
                                ENET_PACKET_FLAG_RELIABLE
                        );
                        enet_peer_send(player_id_to_peer[current_seeker_id], 0, set_state_packet);

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Sending packet CONTROL_SET_PLAYER_STATE to seeker player "
				<< current_seeker_id
				<< " with data:"
				<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
				<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
				<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
				<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
				<< "\n- yaw: " << cspsp_data.state.yaw
				<< "\n- pitch: " << cspsp_data.state.pitch
				<< "\n- flags:"
				<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
				<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
				<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
				<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
				<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
				<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
				<< std::endl;
			#endif // _HNS_DEBUG
			}

			for (auto& [_player_id, player_state] : player_states) {
                                if (_player_id == current_seeker_id) continue;

				player_state.player_state_flags &= ~PlayerStateFlags::IS_SEEKER;
                                player_state.player_state_flags |= PlayerStateFlags::ALIVE;
                                player_state.position = hider_spawn;
                                ControlSetPlayerStatePacketData cspsp_data{};
                                cspsp_data.state = player_state;
                                ENetPacket* set_state_packet = enet_packet_create(
                                        &cspsp_data,
                                        sizeof(ControlSetPlayerStatePacketData),
                                        ENET_PACKET_FLAG_RELIABLE
                                );
                                enet_peer_send(player_id_to_peer[_player_id], 0, set_state_packet);

				#ifdef _HNS_DEBUG
					_DEBUG_LOG
					<< "Sending packet CONTROL_SET_PLAYER_STATE to player "
					<< _player_id
					<< " with data:"
					<< "\n- packet type: " << std::to_string(cspsp_data.packet_type)
					<< "\n- pos X (seeker spawn X): " << cspsp_data.state.position.x
					<< "\n- pos Y (seeker spawn Y): " << cspsp_data.state.position.y
					<< "\n- pos Z (seeker spawn Z): " << cspsp_data.state.position.z
					<< "\n- yaw: " << cspsp_data.state.yaw
					<< "\n- pitch: " << cspsp_data.state.pitch
					<< "\n- flags:"
					<< "\n^ - ALIVE: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::ALIVE) > 0)
					<< "\n^ - IS_SEEKER: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::IS_SEEKER) > 0)
					<< "\n^ - JUMPED: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
					<< "\n^ - WALLJUMP: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
					<< "\n^ - SLIDING: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
					<< "\n^ - FLASHLIGHT: " << ((cspsp_data.state.player_state_flags & PlayerStateFlags::JUMPED) > 0)
					<< "\n- hook_point X: " << cspsp_data.state.hook_point.x
					<< "\n- hook_point Y: " << cspsp_data.state.hook_point.y
					<< "\n- hook_point Z: " << cspsp_data.state.hook_point.z
					<< std::endl;
				#endif // _HNS_DEBUG
                        }

			current_seeker_timer = std::chrono::steady_clock::now();
			game_started = true;

			ENetPacket* game_start_packet = enet_packet_create(
                                std::array<char, 1>{PacketType::CONTROL_GAME_START}.data(),
				sizeof(PacketType),
				ENET_PACKET_FLAG_RELIABLE
			);
			enet_host_broadcast(server, 0, game_start_packet);

			#ifdef _HNS_DEBUG
				_DEBUG_LOG
				<< "Broadcasting packet CONTROL_GAME_START..."
				<< std::endl;
			#endif // _HNS_DEBUG
                }
                break;
        }

        enet_packet_destroy(packet);
}


int main(int argc, char* argv[]) {
try {
        if (argc < 2) {
		std::cout << "USAGE: <PATH/TO/MAP.json> [PORT]" << std::endl;
		return 0;
	}

	std::string map_path = argv[1];
	int port = (argc >= 3) ? std::stoi(argv[2]) : DEFAULT_PORT;

	#ifdef _HNS_DEBUG
		std::cout << "RUNNING DEBUG BUILD; PERFORMANCE WILL BE LOWER" << std::endl;
		std::cout.rdbuf(_DEBUG_LOG.rdbuf());
		std::time_t start_time = std::time(nullptr);
		_DEBUG_LOG << std::asctime(std::localtime(&start_time)) << std::endl;
	#endif // _HNS_DEBUG

	#ifdef _HNS_DEBUG
		_DEBUG_LOG << "map_path: " << map_path << std::endl;
		_DEBUG_LOG << "port: " << port << std::endl;
	#endif // _HNS_DEBUG

        // Map loading, parsing, validation, & compression

	std::ifstream map_file_stream(map_path);
	map_file_stream.seekg(0, std::ios::end);
	map_data.reserve(map_file_stream.tellg());
	map_file_stream.seekg(0, std::ios::beg);
	map_data.assign(
		std::istreambuf_iterator<char>(map_file_stream),
		std::istreambuf_iterator<char>()
	);

	#ifdef _HNS_DEBUG
		_DEBUG_LOG << "map_data size: " << map_data.size() << std::endl;
		_DEBUG_LOG << "map_data: \n'''\n" << map_data << "\n'''\n" << std::endl;
	#endif // _HNS_DEBUG

	if (!nlohmann::json::accept(map_data)) throw std::runtime_error(
		std::string("Map ") + map_path + " is not valid JSON"
	);
	bool map_has_errors = false;
	std::string map_errors("");
	bool hider_spawn_found = false;
	bool seeker_spawn_found = false;
	nlohmann::json _map_data = nlohmann::json::parse(map_data);
	if (!_map_data.is_array()) throw std::runtime_error(
		std::string("Map ") + map_path + " root is not JSON array"
	);
	int map_obj_idx = -1;
	for (
		auto map_obj = _map_data.begin();
		map_obj != _map_data.end();
		map_obj++
	) {
		map_obj_idx++;

		if (!(*map_obj).is_object()) {
			map_errors += (
				"Non-object found in root array at index "
				+ std::to_string(map_obj_idx)
				+ '\n'
			);
			map_has_errors = true;
			continue;
		}

		if (
			!(*map_obj).contains("data") ||
			!(*map_obj).contains("pos") ||
			!(*map_obj).contains("rot") ||
			!(*map_obj).contains("scale") ||
			!(*map_obj).contains("type")
		) {
			map_errors += (
				"Object in root array at index "
				+ std::to_string(map_obj_idx)
				+ " is invalid"
				+ '\n'
			);
			map_has_errors = true;
			continue;
		}

		if ((*map_obj)["type"].get<std::string>().rfind("Spawn_Hider", 0) == 0) {
			hider_spawn_found = true;

			hider_spawn.x = (*map_obj)["pos"][0].get<float>();
			hider_spawn.y = (*map_obj)["pos"][1].get<float>();
			hider_spawn.z = (*map_obj)["pos"][2].get<float>();
		}
		else if ((*map_obj)["type"].get<std::string>().rfind("Spawn_Seeker", 0) == 0) {
			seeker_spawn_found = true;

			seeker_spawn.x = (*map_obj)["pos"][0].get<float>();
			seeker_spawn.y = (*map_obj)["pos"][1].get<float>();
			seeker_spawn.z = (*map_obj)["pos"][2].get<float>();
		}
	}
	if (!hider_spawn_found) {
		map_errors += "Hider_Spawn not found\n";
		map_has_errors = true;
	}
	if (!seeker_spawn_found) {
		map_errors += "Seeker_Spawn not found\n";
		map_has_errors = true;
	}
	if (map_has_errors) throw std::runtime_error(
		std::string("Map ") + map_path + " has errors:\n"
		+ map_errors
	);

	map_data.erase(std::remove_if(map_data.begin(), map_data.end(), []
	(unsigned char c){
		if (
			c == ' ' || c == '\n' || c == '\t'
		) return true;
		return false;
	}), map_data.end());

#ifdef _HNS_DEBUG
	_DEBUG_LOG << "compressed map_data size: " << map_data.size() << std::endl;
	_DEBUG_LOG << "compressed map_data: \n'''\n" << map_data << "\n'''\n" << std::endl;
#endif // _HNS_DEBUG

        // Networking

	if (enet_initialize() != 0) throw std::runtime_error("Failed to initialize ENet");
	atexit(enet_deinitialize);

	ENetAddress address = {0};
	address.host = ENET_HOST_ANY;
	address.port = port;
	server = enet_host_create(&address, MAX_PLAYERS, 1, 0, 0);
	if (server == nullptr) throw std::runtime_error("Failed to create ENet server");
	atexit([]{enet_host_destroy(server);});

	std::cout << "Server started on port " << port << std::endl;

	#ifdef _WIN32
	timeBeginPeriod(1);
	atexit([]{timeEndPeriod(1);});
	#endif

#ifdef _HNS_DEBUG
	_DEBUG_LOG << "\nSERVER STARTED\n" << std::endl;
#endif // _HNS_DEBUG

        ENetEvent event;
        for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

                while (enet_host_service(server, &event, 0) > 0) {
                        switch (event.type) {
                                default: break;

                                case ENET_EVENT_TYPE_CONNECT:
                                {
					#ifdef _HNS_DEBUG
						char _DEBUG_ip[64] = {0};
						enet_address_get_host_ip(
							&event.peer->address,
							_DEBUG_ip,
							sizeof(_DEBUG_ip)
						);
						_DEBUG_LOG
						<< "Received ENET_EVENT_TYPE_CONNECT from "
						<< _DEBUG_ip
						<< std::endl;
					#endif // _HNS_DEBUG

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
					#ifdef _HNS_DEBUG
						char _DEBUG_ip[64] = {0};
						enet_address_get_host_ip(
							&event.peer->address,
							_DEBUG_ip,
							sizeof(_DEBUG_ip)
						);
						_DEBUG_LOG
						<< "Received ENET_EVENT_TYPE_RECEIVE from "
						<< _DEBUG_ip
						<< std::endl;
					#endif // _HNS_DEBUG

                                        HandleReceive(event.peer, event.packet);
                                }
                                break;

                                case ENET_EVENT_TYPE_DISCONNECT:
				#ifdef _HNS_DEBUG
					{
					char _DEBUG_ip[64] = {0};
					enet_address_get_host_ip(
						&event.peer->address,
						_DEBUG_ip,
						sizeof(_DEBUG_ip)
					);
					_DEBUG_LOG
					<< "Received ENET_EVENT_TYPE_DISCONNECT from "
					<< _DEBUG_ip
					<< std::endl;
					}
				#endif // _HNS_DEBUG
                                case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                                {
					if (peer_to_player_id.find(event.peer) == peer_to_player_id.end()) continue;

                                        const PlayerID player_id = peer_to_player_id[event.peer];

                                        if (game_started) {
                                                std::cout
                                                << "Player "
                                                << player_id
                                                << " disconnected during started game"
                                                << ", shutting down..."
                                                << std::endl;

                                                exit(0);
                                        }

                                        std::cout
                                        << "Player "
                                        << player_id
                                        << " disconnected"
                                        << std::endl;

                                        player_states.erase(player_id);
                                        serverside_player_data.erase(player_id);
                                        players_stats.erase(player_id);

                                        player_id_to_peer.erase(player_id);
                                        peer_to_player_id.erase(event.peer);

                                        PlayerDisconnectedPacketData pdp_data{};
					pdp_data.disconnected_player_id = player_id;
					ENetPacket* player_disconnected_packet = enet_packet_create(
						&pdp_data,
						sizeof(PlayerDisconnectedPacketData),
						ENET_PACKET_FLAG_RELIABLE
					);
					enet_host_broadcast(server, 0, player_disconnected_packet);

					#ifdef _HNS_DEBUG
						_DEBUG_LOG
						<< "Broadcasting packet PLAYER_DISCONNECTED with data:\n"
						<< "- Packet type: " << std::to_string(pdp_data.packet_type) << "\n"
						<< "- Disconnected Player ID: " << pdp_data.disconnected_player_id
						<< std::endl;
					#endif // _HNS_DEBUG
                                }
                                break;
                        }
                }
        }

        return 0;
} catch (const std::exception& e) {
	std::cout << "ERROR: " << e.what() << std::endl;
	exit(1);
}
}