#include <iostream>
#include <boost/asio.hpp>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>
#include <map>
#include <sstream>
#include <algorithm> 
#include <chrono>
#include <deque>

using boost::asio::ip::tcp;
using namespace std;

enum class GameMode { NONE, DEATHMATCH, LAST_MAN_STANDING };

const int LMS_NUM_PLAYERS = 3; 
const int INITIAL_HP = 5;

struct Player {
    int id;
    string name;
    shared_ptr<tcp::socket> socket;
    int num_games_played = 0;
    int games_won = 0;
    bool in_match = false;
    int challenged_by = -1;
    char pending_choice = '\0';
    bool challenge_responded = false;
    int hp = INITIAL_HP;
    int current_winstreak = 0;
    int challenge_id = 0;
    GameMode mode = GameMode::NONE;

    float win_ratio() const {
        if (num_games_played == 0) return 0.0f;
        return num_games_played > 0 ? static_cast<double>(games_won) / num_games_played : 0.0;
    }
};

std::atomic<int> global_challenge_id{0};

//A list of active client connections — each shared_ptr<tcp::socket> represents a connected player's socket.
vector<shared_ptr<tcp::socket>> clients;
mutex clients_mutex;
map<int, shared_ptr<Player>> players;
mutex players_mutex;
GameMode selected_mode = GameMode::NONE;

int lms_players_connected = 0;
bool lms_game_started = false;
int deathmatch_players_connected = 0;
bool deathmatch_game_started = false;

void broadcast(const string& message, shared_ptr<tcp::socket> sender) {
    lock_guard<mutex> lock(clients_mutex);
    //Without this lock, a client disconnecting mid-broadcast could crash the server.
    for (auto& client : clients) {
        if (client != sender) {
            try {
                boost::asio::write(*client, boost::asio::buffer(message));
            } catch (...) {
                // Silently fail if client is disconnected
            }
        }
    }
}
void broadcast_to_all(const string& message) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& client : clients) {
        try {
            boost::asio::write(*client, boost::asio::buffer(message));
        } catch (...) {
            // Ignore disconnects
        }
    }
}

void broadcast_player_status() {
    lock_guard<mutex> lock(players_mutex);
    string list = "[PLAYER_STATUS]\n";
    for (auto& [id, p] : players) {
        try{
            if (!p) {
                cerr << "[ERROR] Null player pointer for ID: " << id << endl;
                continue;
            }
            ostringstream oss;
            oss.precision(2);
            oss << fixed << p->win_ratio();
            list += p->name + " | Games: " + to_string(p->num_games_played)  +
                " | Ratio: " + oss.str() + " | HP: " + to_string(p->hp) + "\n";
        } catch (const std::exception& e) {
            cerr << "Exception while broadcasting player status: " << e.what() << endl;
        }
        
    }
 
    list += "[PLAYER_STATUS_END]\n";
    broadcast_to_all(list);
}
void check_lms_game_end() {
    int alive = 0;
    string last_alive;
    for (auto& [id, p] : players) {
        if (p->mode == GameMode::LAST_MAN_STANDING && p->hp > 0) {
            alive++;
            last_alive = p->name;
        }
    }
    if (alive == 1 && lms_game_started) {
        string msg = "Game Over! Winner: " + last_alive + "\n";
        broadcast(msg, nullptr);
        lms_game_started = false;

        broadcast_player_status();
    }
}

void challenge_timeout(shared_ptr<Player> challenger, shared_ptr<Player> challenged,  int challenge_id) {
    this_thread::sleep_for(chrono::seconds(10));
    // If the challenged player hasn't responded, challenger wins by timeout
    bool timeoutOccurred = false;
    {
        lock_guard<mutex> lock(players_mutex);
        if (challenger->in_match && challenged->in_match 
            && challenger->challenge_id == challenge_id 
            && challenged->challenge_id == challenge_id 
            && !challenged->challenge_responded) {
    
            timeoutOccurred = true;
            challenger->num_games_played++;
            challenged->num_games_played++;
            
            if (selected_mode == GameMode::LAST_MAN_STANDING && lms_game_started) {
                challenger->games_won++;
                challenger->current_winstreak++;
                challenged->hp--;
                string timeout_msg = challenged->name + " lost 1 HP by timeout! Remaining HP: " + to_string(challenged->hp) + "\n";
                broadcast(timeout_msg, nullptr);
                if (challenger->current_winstreak == 3 && challenger->hp > 0) {
                    challenger->hp++;
                    broadcast("🔥 Killing Spree! " + challenger->name + " gains +1 HP! 🔥\n", nullptr);
                    challenger->current_winstreak = 0;
                }
                if (challenged->hp <= 0) {
                    string dead_msg = challenged->name + " has died.\n";
                    broadcast(dead_msg, nullptr);
                    challenged->in_match = true;
                }
                check_lms_game_end();
                
            } else {
               
                challenger->games_won++;
                string timeout_msg = "Match Result: " + challenger->name + " wins by timeout (no response from " +
                challenged->name + ")\n";
                broadcast(timeout_msg, nullptr);
                
    
                challenged->in_match = false;
                challenged->challenged_by = -1;
                challenged->challenge_responded = false;
             
            }
            
    
            // Reset match state
            challenger->in_match = false;
            challenger->pending_choice = '\0';
            
        }
    }
    if (timeoutOccurred) {
        broadcast_player_status(); 
    }
}

void handle_client(shared_ptr<tcp::socket> socket, int player_id) {
    //streambuf contains a chunk of memory that you can use to temporarily hold input or output data.
    boost::asio::streambuf buffer;
    //This line creates an input stream (like cin) that reads from the streambuf.
    istream input(&buffer);
   
    //This section safely adds the new player's socket to the global clients list.
   //lock_guard<mutex> ensures that this operation is thread-safe — only one thread can modify clients at a time.
    {
        lock_guard<mutex> lock(clients_mutex);
        clients.push_back(socket);
    }

    // Register the player
    auto player = make_shared<Player>();
    player->id = player_id;
    player->name = "Player" + to_string(player_id);
    player->socket = socket;

    {
        // This ensures only one thread at a time can modify or read from players.
        lock_guard<mutex> lock(players_mutex);
        players[player_id] = player;
    }

    string join_msg = player->name + " joined the chat.\n";
    broadcast(join_msg, socket);
    cout << "Player " << player->name << " joined the chat." << endl << std::flush; 

    string welcome = "Rock-Paper-Scissors Battle Arena\n";
    boost::asio::write(*socket, boost::asio::buffer(welcome));
    
    try {
        while (true) {
            boost::asio::read_until(*socket, buffer, '\n');
            string msg;
            getline(input, msg);

            if (!msg.empty()) {

                if (msg.rfind("/mode", 0) == 0) {
                    if (player->in_match || player->challenged_by != -1) {
                        string err = "You cannot switch modes while in a match!\n";
                        boost::asio::write(*socket, boost::asio::buffer(err));
                        continue;
                    }
                    istringstream ss(msg);
                    string cmd, mode_choice;
                    ss >> cmd >> mode_choice;

                    if (mode_choice == "deathmatch") {
                        player->mode = GameMode::DEATHMATCH;
                        selected_mode = GameMode::DEATHMATCH;
                        deathmatch_players_connected++;

                        if (deathmatch_players_connected >= 2 && !deathmatch_game_started) {
                            deathmatch_game_started = true;
                            broadcast("Deathmatch: At least 2 players connected. Game starting!\n", nullptr);
                        }
                    } else if (mode_choice == "lms") {
                        player->mode = GameMode::LAST_MAN_STANDING;
                        selected_mode = GameMode::LAST_MAN_STANDING;
                        player->hp = INITIAL_HP;
                        lms_players_connected++;
                        if (lms_players_connected == LMS_NUM_PLAYERS) {
                            lms_game_started = true;
                            broadcast("LMS: All players connected. Game starting!\n", nullptr);
                        }
                    }
                    string join_msg = player->name + " joined in mode: " + mode_choice + "\n";
                    broadcast(join_msg, socket);
                }
                // Command: /challenge <id> <R|P|S>
                else if (msg.rfind("/challenge", 0) == 0) {
                    istringstream ss(msg);
                    string cmd;
                    int target_id;
                    char move;
                    ss >> cmd >> target_id >> move;

                    lock_guard<mutex> lock(players_mutex);
                    if (players.count(target_id) && players[target_id]->hp > 0) {
                        if (target_id == player->id) {
                            boost::asio::write(*socket, boost::asio::buffer("You can't challenge yourself.\n"));
                            continue;
                        }
                        if (players[target_id]->in_match || player->in_match || player->hp <= 0 || players[target_id]->hp <= 0) {
                            string err_msg = "Cannot challenge. Player is busy or dead. " + players[target_id]->name + " is currently in a match or dead.\n";
                            boost::asio::write(*socket, boost::asio::buffer(err_msg));
                        } else {
                            auto target = players[target_id];
                            player->challenged_by = target_id;
                            target->challenged_by = player_id;
                            player->in_match = true;
                            target->in_match = true;
                            player->pending_choice = move;
                            target->challenge_responded = false;
                            int challenge_id = ++global_challenge_id;
                            player->challenge_id = challenge_id;
                            target->challenge_id = challenge_id;
                        
                            // Start a timeout thread
                            thread(challenge_timeout, player, target, challenge_id).detach();

                            // Send challenge message
                            string challenge_msg = "Challenge: " + player->name + " challenged you to a game. Timeout in 10 seconds\n";
                            boost::asio::write(*target->socket, boost::asio::buffer(challenge_msg));
                        }
                    }
                
            }
                // Command: /move <R|P|S>
                else if (msg.rfind("/move", 0) == 0) {
                    istringstream ss(msg);
                    string cmd;
                    char reply_move;
                    ss >> cmd >> reply_move;

                    if (player->challenged_by != -1) {
                        lock_guard<mutex> lock(players_mutex);
                        auto challenger = players[player->challenged_by];
                        char initiator_move = challenger->pending_choice;

                        string result;
                        if (initiator_move == reply_move) result = "draw";
                        else if ((initiator_move == 'R' && reply_move == 'S') ||
                                 (initiator_move == 'P' && reply_move == 'R') ||
                                 (initiator_move == 'S' && reply_move == 'P')) {
                            result = "initiator";
                        } else {
                            result = "challenged";
                        }

                        challenger->num_games_played++;
                        player->num_games_played++;

                        if (selected_mode == GameMode::LAST_MAN_STANDING && lms_game_started) {
                            if (result == "initiator") {
                                player->hp--;
                                challenger->current_winstreak++;
                                player->current_winstreak = 0;
                                challenger->games_won++;

                                if (challenger->current_winstreak == 3 && challenger->hp > 0) {
                                    challenger->hp++;
                                    broadcast("🔥 Killing Spree! " + challenger->name + " gains +1 HP! 🔥\n", nullptr);
                                    challenger->current_winstreak = 0;
                                }
                            } else if (result == "challenged") {
                                challenger->hp--;
                                player->current_winstreak++;
                                challenger->current_winstreak = 0;
                                player->games_won++;

                                if (player->current_winstreak == 3 && player->hp > 0) {
                                    player->hp++;
                                    broadcast("🔥 Killing Spree! " + player->name + " gains +1 HP! 🔥\n", nullptr);
                                    player->current_winstreak = 0;
                                }
                            } else {
                                challenger->current_winstreak = 0;
                                player->current_winstreak = 0;
                            }
                           
                            if (challenger->hp <= 0){
                                broadcast(challenger->name + " has died.\n", nullptr);
                            } 
                            if (player->hp <= 0){
                                broadcast(player->name + " has died.\n", nullptr);
                            } 

                            check_lms_game_end();
                        } else {
                            if (result == "initiator") challenger->games_won++;
                            else if (result == "challenged") player->games_won++;
                        }

                        
                        string summary = "Match Result: " + challenger->name + " (" + initiator_move + ") vs "
                                         + player->name + " (" + reply_move + ") — ";
                        summary += (result == "draw" ? "It's a draw!" :
                                    (result == "initiator" ? challenger->name + " wins!" : player->name + " wins!")) + "\n";

                        broadcast(summary, nullptr);
                        
                        // Reset challenge state
                        challenger->in_match = false;
                        player->in_match = false;
                        challenger->pending_choice = '\0';
                        player->challenged_by = -1;
                        challenger->challenge_responded = false;
                        player->challenge_responded = true;


                    }
                    broadcast_player_status();

                }

                // Otherwise: treat it as chat
                else {
                    string full_msg = "[" + player->name + "]: " + msg + "\n";
                    broadcast(full_msg, socket);
                }
            }
        }
    } catch (const std::exception& e) {
        cerr << "Exception while handling client: " << e.what() << endl;
    }

    // Handle client disconnection or termination
    string leave_msg = player->name + " left the chat.\n";
    broadcast(leave_msg, socket);

    {
        lock_guard<mutex> lock(clients_mutex);
        clients.erase(remove(clients.begin(), clients.end(), socket), clients.end());
    }

    {
        lock_guard<mutex> lock(players_mutex);
        players.erase(player_id);
    }
}

int main() {
    try {
        boost::asio::io_service io_service;

        tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), 12345));
        std::cout << "Server listening on port 12345..." << std::endl;

        int player_id = 1;

        //Inside an infinite loop, the server waits for new clients, Player threads can keep playing even if server loop continues.
        while (true) {
            shared_ptr<tcp::socket> socket = make_shared<tcp::socket>(io_service);
            acceptor.accept(*socket);

            // Handle each client connection in a separate thread
            thread(handle_client, socket, player_id++).detach();
        }


    } catch (const exception& e) {
        std::cerr << "Server exception: " << e.what() << std::endl;
    }

    return 0;
}
