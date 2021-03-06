#include "stdafx.h"

#include "client.h"
#include "client_dialog.h"
#include "connection.h"
#include "util.h"
#include "uri.h"
#include <dirent.h>
#include <cryptopp/sha.h>
#include <cryptopp/files.h>
#include <cryptopp/filters.h>
#include <cryptopp/base64.h>
#include <cryptopp/hex.h>

using namespace std;
using namespace asio;


int get_input_rate(char code) {
    switch (code) {
        case BRAZILIAN:
        case CHINESE:
        case GERMAN:
        case FRENCH:
        case DUTCH:
        case ITALIAN:
        case GATEWAY_64_PAL:
        case EUROPEAN_BASIC_SPEC:
        case SPANISH:
        case AUSTRALIAN:
        case SCANDINAVIAN:
        case EUROPEAN_X:
        case EUROPEAN_Y:
            return 50;
        default:
            return 60;
    }
}

bool operator==(const BUTTONS& lhs, const BUTTONS& rhs) {
    return lhs.Value == rhs.Value;
}

bool operator!=(const BUTTONS& lhs, const BUTTONS& rhs) {
    return !(lhs == rhs);
}

client::client(shared_ptr<client_dialog> dialog) :
    connection(service), udp_resolver(service), timer(service), my_dialog(dialog)
{
    my_dialog->set_message_handler([=](string message) {
        service.post([=] { on_message(message); });
    });

    my_dialog->set_close_handler([=] {
        service.post([=] {
            if (started) {
                my_dialog->minimize();
            } else {
                my_dialog->destroy();
                close();
                map_src_to_dst();
                start_game();
            }
        });
    });

    my_dialog->info(
        "Available Commands:\r\n\r\n"
        "/name <name> ................ Set your name\r\n"
        "/host [port] ................ Host a private server\r\n"
        "/join <address> ............. Join a game\r\n"
        "/start ...................... Start the game\r\n"
        "/mode ....................... Toggle your input authority mode\r\n"
        "/map <slot> <slot> [...] .... Map local controllers to netplay\r\n"
        "/autolag .................... Toggle automatic lag on and off\r\n"
        "/lag <lag> .................. Set the netplay input lag\r\n"
        "/golf ....................... Toggle golf mode\r\n"
        "/favorite <address> ......... Add this server and room to favorites\r\n"
        "/unfavorite <name> .......... Remove server from favorites list\r\n"
        "/savesync <name> ............ Syncronize game save from someone\r\n"
        "/roomcheck .................. Check if room is compatible\r\n"
    );

#ifdef DEBUG
    input_log.open("input.log");
#endif
}

client::~client() {
    stop();
}

void client::on_error(const error_code& error) {
    if (error) {
        my_dialog->error(error == error::eof ? "Disconnected from server" : error.message());
    }
}

void client::add_server(std::string& server) {
    public_servers[server] = SERVER_STATUS_PENDING;
}

void client::remove_server(std::string& server) {
    public_servers.erase(server);
}

void client::load_public_server_list() {
    constexpr static char API_HOST[] = "api.play64.com";

    ip::tcp::resolver tcp_resolver(service);
    error_code error;
    auto iterator = tcp_resolver.resolve(ip::tcp::resolver::query(API_HOST, "80"), error);
    if (error) return my_dialog->error("Failed to load server list");
    auto s = make_shared<ip::tcp::socket>(service);
    s->async_connect(*iterator, [=](const error_code& error) {
        if (error) return my_dialog->error("Failed to load server list");
        shared_ptr<string> buf = make_shared<string>(
            "GET /server-list.txt HTTP/1.1\r\n"
            "Host: " + string(API_HOST) + "\r\n"
            "Connection: close\r\n\r\n");
        async_write(*s, buffer(*buf), [=](const error_code& error, size_t transferred) {
            if (error) {
                s->close();
                my_dialog->error("Failed to load server list");
            } else {
                buf->resize(4096);
                async_read(*s, buffer(*buf), [=](const error_code& error, size_t transferred) {
                    s->close();
                    if (error != error::eof) return my_dialog->error("Failed to load server list");
                    buf->resize(transferred);
                    public_servers.clear();
                    bool content = false;
                    for (size_t start = 0, end = 0; end != string::npos; start = end + 1) {
                        end = buf->find('\n', start);
                        string line = buf->substr(start, end == string::npos ? string::npos : end - start);
                        if (!line.empty() && line.back() == '\r') {
                            line.resize(line.length() - 1);
                        }
                        if (line.empty()) {
                            content = true;
                        } else if (content) {
                            public_servers[line] = SERVER_STATUS_PENDING;
                        }
                    }
                    public_servers[me->favorite_server] = SERVER_STATUS_PENDING;
                    my_dialog->update_server_list(public_servers);
                    ping_public_server_list();
                });
            }
        });
    });
}

void client::ping_public_server_list() {
    auto done = [=](const string& host, double ping, shared_ptr<ip::udp::socket> socket = nullptr) {
        public_servers[host] = ping;
        my_dialog->update_server_list(public_servers);
        if (socket && socket->is_open()) {
            error_code error;
            socket->shutdown(ip::tcp::socket::shutdown_both, error);
            socket->close(error);
        }
    };
    for (auto& e : public_servers) {
        auto host = e.first;
        uri u(host);
        udp_resolver.async_resolve(ip::udp::resolver::query(u.host, to_string(u.port ? u.port : 6400)), [=](const auto& error, auto iterator) {
            if (error) return done(host, SERVER_STATUS_ERROR);
            auto socket = make_shared<ip::udp::socket>(service);
            
            socket->open(iterator->endpoint().protocol());
            socket->connect(*iterator);
            auto p(make_shared<packet>());
            *p << PING << timestamp();
            socket->async_send(buffer(*p), [=](const error_code& error, size_t transferred) {
                p->reset();
                if (error) return done(host, SERVER_STATUS_ERROR, socket);

                auto timer = make_shared<asio::steady_timer>(service);
                timer->expires_after(std::chrono::seconds(3));
                
                auto self(weak_from_this());
                socket->async_wait(ip::udp::socket::wait_read, [=](const error_code& error) {
                    if (self.expired()) return;
                    timer->cancel();
                    if (error) return done(host, SERVER_STATUS_ERROR, socket);
                    error_code ec;
                    p->resize(socket->available(ec));
                    if (ec) return done(host, SERVER_STATUS_ERROR, socket);
                    p->resize(socket->receive(buffer(*p), 0, ec));
                    if (ec) return done(host, SERVER_STATUS_ERROR, socket);
                    if (p->size() < 13 || p->read<packet_type>() != PONG || p->read<uint32_t>() != PROTOCOL_VERSION) {
                        return done(host, SERVER_STATUS_VERSION_MISMATCH, socket);
                    }
                    done(host, timestamp() - p->read<double>(), socket);
                });

                timer->async_wait([timer, socket](const asio::error_code& error) {
                    if (socket && socket->is_open()) {
                        error_code error;
                        socket->shutdown(ip::tcp::socket::shutdown_both, error);
                        socket->close(error);
                    }
                });
            });
        });
    }
}

string client::get_name() {
    return run([&] { return me->name; });
}

string client::get_favorite_server() {
    return run([&] { return me->favorite_server; });
}

void client::set_name(const string& name) {
    run([&] {
        me->name = name;
        trim(me->name);
        my_dialog->info("Your name is " + name);
    });
}

void client::set_rom_info(const rom_info& rom) {
    run([&] {
        me->rom = rom;
        my_dialog->info("Your game is " + me->rom.to_string());
    });
}

void client::set_save_info(const string& save_path) {
    this->save_path = save_path;
    run([&] {
        update_save_info();
    });
}

void client::set_favorite_server(const string& fav_server) {
    run([&] {
        me->favorite_server = fav_server;
        trim(me->favorite_server);
        my_dialog->info("Your favorite server is: " + fav_server);
    });
}


void client::set_src_controllers(CONTROL controllers[4]) {
    for (int i = 0; i < 4; i++) {
        controllers[i].RawData = FALSE; // Disallow raw data
    }

    run([&] {
        for (int i = 0; i < 4; i++) {
            me->controllers[i].plugin = controllers[i].Plugin;
            me->controllers[i].present = controllers[i].Present;
            me->controllers[i].raw_data = controllers[i].RawData;
        }
        send_controllers();
    });
}

void client::set_dst_controllers(CONTROL controllers[4]) {
    run([&] { this->controllers = controllers; });
}

void client::process_input(array<BUTTONS, 4>& buttons) {
    run([&] {
#ifdef DEBUG
        //static uniform_int_distribution<uint32_t> dist(16, 63);
        //static random_device rd;
        //static uint32_t i = 0;
        //while (input_id >= i) i += dist(rd);
        //if (golf) input[0].A_BUTTON = (i & 1);
#endif
        input_data input = { buttons[0].Value, buttons[1].Value, buttons[2].Value, buttons[3].Value, me->map };
        if (me->input_authority == HOST) {
            send_hia_input(input);
            if (golf && input) {
                set_input_authority(CLIENT);
                pending_cia_input = input;
            }
        } else if (me->input_authority == CLIENT && (me->input_id - input_id) < me->lag) {
            send_input(input);
            send_input(input);
        } else if (me->input_authority == CLIENT && (me->input_id - input_id) == me->lag || input_id % 2) {
            send_input(input);
        }
        
        on_input();
    });
    
    unique_lock<mutex> lock(next_input_mutex);
    next_input_condition.wait(lock, [=] { return !next_input.empty(); });
    buttons = next_input.front();
    next_input.pop_front();
}

void client::on_input() {
    for (auto& u : user_list) {
        if (u->input_queue.empty()) {
            return;
        }
    }

    unique_lock<mutex> lock(next_input_mutex);
    if (!next_input.empty()) {
        return;
    }

    array<BUTTONS, 4> result = { 0, 0, 0, 0 };
    array<int16_t, 4> analog_x = { 0, 0, 0, 0 };
    array<int16_t, 4> analog_y = { 0, 0, 0, 0 };

    for (auto& u : user_list) {
        if (u->input_queue.empty()) continue;
        auto input = u->input_queue.front();
        u->input_queue.pop_front();
        assert(u->input_id > input_id);

        auto b = input.map.bits;
        for (int i = 0; b && i < 4; i++) {
            BUTTONS buttons = { input.data[i] };
            for (int j = 0; b && j < 4; j++, b >>= 1) {
                if (b & 1) {
                    result[j].Value |= buttons.Value;
                    analog_x[j] += buttons.X_AXIS;
                    analog_y[j] += buttons.Y_AXIS;
                }
            }
        }
    }
    
    for (int i = 0; i < 4; i++) {
        double x = analog_x[i] + 0.5;
        double y = analog_y[i] + 0.5;
        double r = max(abs(x), abs(y));
        if (r > 127.5) {
            result[i].X_AXIS = (int)floor(x / r * 127.5);
            result[i].Y_AXIS = (int)floor(y / r * 127.5);
        } else {
            result[i].X_AXIS = analog_x[i];
            result[i].Y_AXIS = analog_y[i];
        }
    }

    next_input.push_back(result);
    next_input_condition.notify_one();

#ifdef DEBUG
    constexpr static char B[] = "><v^SZBA><v^RL";
    static array<BUTTONS, 4> prev = { 0, 0, 0, 0 };
    if (input_log.is_open() && result != prev) {
        input_log << setw(8) << setfill('0') << input_id << '|';
        for (int i = 0; i < 4; i++) {
            (result[i].X_AXIS ? input_log << setw(4) << setfill(' ') << showpos << result[i].X_AXIS << ' ' : input_log << "     ");
            (result[i].Y_AXIS ? input_log << setw(4) << setfill(' ') << showpos << result[i].Y_AXIS << ' ' : input_log << "     ");
            for (int j = static_cast<int>(strlen(B)) - 1; j >= 0; j--) {
                input_log << (result[i].Value & (1 << j) ? B[j] : ' ');
            }
            input_log << '|';
        }
        input_log << '\n';
    }
    prev = result;
#endif

    input_id++;
}

void client::post_close() {
    service.post([&] { close(); });
}

void client::wait_until_start() {
    if (started) return;
    unique_lock<mutex> lock(start_mutex);
    start_condition.wait(lock, [=] { return started; });
}

void client::on_message(string message) {
    try {
        if (message.substr(0, 1) == "/") {
            vector<string> params;
            for (auto start = message.begin(), end = start; start != message.end(); start = end) {
                start = find_if(start, message.end(), [](char ch) { return !isspace<char>(ch, locale::classic()); });
                end   = find_if(start, message.end(), [](char ch) { return  isspace<char>(ch, locale::classic()); });
                if (end > start) {
                    params.push_back(string(start, end));
                }
            }

            if (params[0] == "/name") {
                if (params.size() < 2) throw runtime_error("Missing parameter");

                me->name = params[1];
                trim(me->name);
                my_dialog->info("Your name is now " + me->name);
                send_name();
            } else if (params[0] == "/host" || params[0] == "/server") {
                if (started) throw runtime_error("Game has already started");

                host = "127.0.0.1";
                port = params.size() >= 2 ? stoi(params[1]) : 6400;
                path = "/";
                close();
                my_server = make_shared<server>(service, false);
                port = my_server->open(port);
                my_dialog->info("Server is listening on port " + to_string(port) + "...");
                connect(host, port, path);
            } else if (params[0] == "/join" || params[0] == "/connect") {
                if (started) throw runtime_error("Game has already started");
                if (params.size() < 2) throw runtime_error("Missing parameter");

                uri u(params[1]);
                if (!u.scheme.empty() && u.scheme != "play64") {
                    throw runtime_error("Unsupported protocol: " + u.scheme);
                }
                host = u.host;
                port = params.size() >= 3 ? stoi(params[2]) : (u.port ? u.port : 6400);
                path = u.path;
                close();
                connect(host, port, path);
            } else if (params[0] == "/start") {
                if (started) throw runtime_error("Game has already started");

                if (is_open()) {
                    send_start_game();
                } else {
                    map_src_to_dst();
                    set_lag(0);
                    start_game();
                }
            } else if (params[0] == "/lag") {
                if (params.size() < 2) throw runtime_error("Missing parameter");
                uint8_t lag = stoi(params[1]);
                if (!is_open()) throw runtime_error("Not connected");
                send_autolag(0);
                send_lag(lag, true, true);
                set_lag(lag);
            } else if (params[0] == "/my_lag") {
                if (params.size() < 2) throw runtime_error("Missing parameter");
                uint8_t lag = stoi(params[1]);
                send_lag(lag, true, false);
                set_lag(lag);
            } else if (params[0] == "/your_lag") {
                if (params.size() < 2) throw runtime_error("Missing parameter");
                if (!is_open()) throw runtime_error("Not connected");
                uint8_t lag = stoi(params[1]);
                send_autolag(0);
                send_lag(lag, false, true);
            } else if (params[0] == "/autolag") {
                if (!is_open()) throw runtime_error("Not connected");
                send_autolag();
            } else if (params[0] == "/golf") {
                if (!is_open()) throw runtime_error("Not connected");
                set_golf_mode(!golf);
                send(packet() << GOLF << golf);
            } else if (params[0] == "/map") {
                if (!is_open()) throw runtime_error("Not connected");

                input_map map;
                for (size_t i = 2; i < params.size(); i += 2) {
                    int src = stoi(params[i - 1]) - 1;
                    int dst = stoi(params[i]) - 1;
                    map.set(src, dst);
                }
                set_input_map(map);
            } else if (params[0] == "/mode") {
                if (!is_open()) throw runtime_error("Not connected");
                set_input_authority(me->input_authority == HOST ? CLIENT : HOST);
            } else if (params[0] == "/hia_rate") {
                if (params.size() < 2) throw runtime_error("Missing parameter");
                if (!is_open()) throw runtime_error("Not connected");
                uint32_t rate = stoi(params[1]);
                send(packet() << HIA_RATE << rate);
            } else if (params[0] == "/favorite") {
                if (params.size() < 2) throw runtime_error("Missing parameter");
                public_servers.erase(me->favorite_server);

                me->favorite_server = params[1];
                trim(me->favorite_server);
                add_server(me->favorite_server);
                my_dialog->info("Added " + me->favorite_server + " to favorites");
                my_dialog->update_server_list(public_servers);
            } else if (params[0] == "/unfavorite") {
                public_servers.erase(me->favorite_server);
                me->favorite_server = "<-- No Favorite Set -->";
                public_servers[me->favorite_server] = SERVER_STATUS_PENDING;
                my_dialog->update_server_list(public_servers);
            } else if (params[0] == "/roomcheck") {
                send(packet() << ROOM_CHECK);
            } else if (params[0] == "/savesync") {
                if (params.size() < 1) throw runtime_error("Missing parameter");
                std::string name = params.size() < 2 ? "": params[1];
                trim(name);
                if (me->name == name) {
                    my_dialog->error("Don't pick yourself for a savesync");
                    return;
                }
                
                if (name == "" || name == "all") {
                    my_dialog->info("Syncing your save with everyone else");
                    
                    send_savesync();
                    return;
                }

                std::array<save_info,5> saves;
                bool found = false;
                for (auto& user : user_map) {
                    if (user->name == name) {
                        saves = user->saves;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    my_dialog->info("Could not find any user named " + name);
                    return;
                }

                for (unsigned int i = 0; i < me->saves.size(); i++) {
                    auto& save_data = saves[i];
                    if(me->saves[i].sha1_data != save_data.sha1_data)
                        replace_save_file(save_data);
                }

                update_save_info();
                send_save_info();
            } else {
                throw runtime_error("Unknown command: " + params[0]);
            }
        } else {
            my_dialog->message(me->name, message);
            send_message(message);
        }
    } catch (const exception& e) {
        my_dialog->error(e.what());
    } catch (const error_code& e) {
        my_dialog->error(e.message());
    }
}

void client::set_lag(uint8_t lag) {
    me->lag = lag;

    //my_dialog->set_lag(lag);
}

void client::set_input_authority(application authority, application initiator) {
    if (authority == me->input_authority) return;

    if (authority == HOST || initiator == HOST) {
        me->input_authority = authority;
        if (authority == CLIENT) {
            send_input(pending_cia_input);
            send_input(pending_cia_input);
            pending_cia_input = input_data();
            on_input();
        }
    }

    if (authority == HOST || initiator == CLIENT) {
        send(packet() << INPUT_AUTHORITY << authority);
    }
}

void client::set_golf_mode(bool golf) {
    if (golf == this->golf) return;
    this->golf = golf;
    if (golf) {
        my_dialog->info("Golf mode is enabled");
    } else {
        my_dialog->info("Golf mode is disabled");
    }
}

void client::remove_user(uint32_t user_id) {
    if (user_map.at(user_id) == me) return;

    if (started) {
        my_dialog->error(user_map.at(user_id)->name + " has quit");
    } else {
        my_dialog->info(user_map.at(user_id)->name + " has quit");
    }

    user_map.at(user_id) = nullptr;

    user_list.clear();
    for (auto& u : user_map) {
        if (u) user_list.push_back(u);
    }

    update_user_list();
}

void client::message_received(uint32_t user_id, const string& message) {
    switch (user_id) {
        case ERROR_MSG:
            my_dialog->error(message);
            break;

        case INFO_MSG:
            my_dialog->info(message);
            break;

        default:
            my_dialog->message(user_map.at(user_id)->name, message);
            break;
    }
}

void client::close() {
    connection::close();

    timer.cancel();

    if (my_server) {
        my_server->close();
        my_server.reset();
    }

    user_map.clear();
    user_map.push_back(me);
    user_list.clear();
    user_list.push_back(me);

    me->lag = 0;
    me->input_authority = CLIENT;

    if (started) {
        send_input(input_data());
        on_input();
    }

    update_user_list();
}

void client::start_game() {
    unique_lock<mutex> lock(start_mutex);
    if (started) return;
    started = true;
    start_condition.notify_all();
    my_dialog->info("Starting game...");
}

void client::connect(const string& host, uint16_t port, const string& room) {
    my_dialog->info("Connecting to " + host + (port == 6400 ? "" : ":" + to_string(port)) + "...");

    ip::tcp::resolver tcp_resolver(service);
    error_code error;
    auto endpoint = tcp_resolver.resolve(ip::tcp::resolver::query(host, to_string(port)), error);
    if (error) {
        return my_dialog->error(error.message());
    }

    if (!tcp_socket) {
        tcp_socket = make_shared<ip::tcp::socket>(service);
    }
    tcp_socket->connect(*endpoint, error);
    if (error) {
        return my_dialog->error(error.message());
    }
    tcp_socket->set_option(ip::tcp::no_delay(true), error);
    if (error) {
        return my_dialog->error(error.message());
    }


    try {
        if (!udp_socket) {
            udp_socket = make_shared<ip::udp::socket>(service);
        }
        auto udp_endpoint = ip::udp::endpoint(tcp_socket->local_endpoint().address(), 0);
        udp_socket->open(udp_endpoint.protocol());
        udp_socket->bind(udp_endpoint);
    } catch (error_code e) {
        if (udp_socket) {
            udp_socket->close(e);
            udp_socket.reset();
        }
    }

    my_dialog->info("Connected!");

    send_join(room);

    receive_tcp_packet();
}

void client::replace_save_file(const save_info& save_data) {
    //todo rename the original file if it was found

    //std::ifstream in(save_path + saveInfo.save_name);
    //bool delete_file = in.good();

    //in.close();
    //if (delete_file)
    if (!save_data.save_name.empty()) {
        DeleteFileA((save_path + save_data.save_name).c_str());

        std::ofstream of((save_path + save_data.save_name).c_str(), std::ofstream::binary);
        of << save_data.save_data;
        of.close();
    }
}

std::vector<string> client::find_rom_save_files(const string& rom_name) {
    struct dirent* entry;
    DIR* dp;
    std::vector<string> ret;

    dp = opendir(save_path.c_str());
    if (dp == NULL)
        return ret;
    

    std::regex accept = std::regex("\\b((\\w+\\s?)+\\.((sra)|(eep)|(fla)|(mpk)))");
    while ((entry = readdir(dp)) != NULL) {
        if (entry->d_type && entry->d_type == DT_DIR) {
            continue;
        }

        std::string filename = std::string(entry->d_name);
        if (filename.find(rom_name) != std::string::npos
            && std::regex_search(entry->d_name, accept)) {
            ret.push_back(filename);
        }
    }
    closedir(dp);
    return ret;
}

string client::slurp(const string& path) {
    ostringstream buffer;
    ifstream input(path.c_str());
    buffer << input.rdbuf();
    return buffer.str();
}

string client::slurp2(const string& path) {
    std::ifstream t(path.c_str(), std::ios::binary);
    std::string str;

    t.seekg(0, std::ios::end);
    str.reserve(t.tellg());
    t.seekg(0, std::ios::beg);

    str.assign((std::istreambuf_iterator<char>(t)),
        std::istreambuf_iterator<char>());
    return str;
}

void client::update_save_info()
{
    std::vector<string> save_files = find_rom_save_files(me->rom.name);
    if(save_files.size() == 0)
        my_dialog->info("Save Data is empty, you'll need to savesync with another player");

    std::sort(save_files.begin(), save_files.end());

    while (me->saves.size() > save_files.size()) {
        save_files.push_back("");
    }
    
    for (int i = 0; i < me->saves.size(); i++) {
        auto& save_info = me->saves[i];
        save_info.rom_name = me->rom.name;
        save_info.save_name = save_files.at(i);
        save_info.save_data = save_info.save_name.empty() ? "" : slurp2(save_path + save_info.save_name);
        save_info.sha1_data = save_info.save_name.empty() ? "" : sha1_save_info(save_info);

        if (!save_info.save_name.empty())
            my_dialog->info("Save Data: " + save_info.save_name + "\r\nhash: " + save_info.sha1_data);
    }    
}


string client::sha1_save_info(const save_info& saveInfo)
{
    CryptoPP::SHA256 hash;
    string digest;
    string filename = (save_path + saveInfo.save_name);
    CryptoPP::FileSource file(filename.c_str(), true, new CryptoPP::HashFilter(hash,
        new CryptoPP::HexEncoder(
            new CryptoPP::StringSink(digest)))
    );
    return digest;
}

void client::on_receive(packet& p, bool reliable) {
    switch (p.read<packet_type>()) {
        case VERSION: {
            auto protocol_version = p.read<uint32_t>();
            if (protocol_version != PROTOCOL_VERSION) {
                close();
                my_dialog->error("Server protocol version does not match client protocol version. Visit www.play64.com to get the latest version of the plugin.");
            }
            break;
        }
        case JOIN: {
            auto info = p.read<user_info>();
            my_dialog->info(info.name + " has joined");

            auto u = make_shared<user_info>(info);
            user_map.push_back(u);
            user_list.push_back(u);
            update_user_list();
            break;
        }

        case ACCEPT: {
            auto udp_port = p.read<uint16_t>();
            if (udp_socket && udp_port) {
                udp_socket->connect(ip::udp::endpoint(tcp_socket->remote_endpoint().address(), udp_port));
                receive_udp_packet();
            } else {
                udp_socket.reset();
            }
            on_tick();

            user_map.clear();
            user_list.clear();
            while (p.available()) {
                if (p.read<bool>()) {
                    auto u = make_shared<user_info>(p.read<user_info>());
                    user_map.push_back(u);
                    user_list.push_back(u);
                } else {
                    user_map.push_back(nullptr);
                }
            }
            user_map.push_back(me);
            user_list.push_back(me);
            break;
        }

        case PATH: {
            path = p.read<string>();
            my_dialog->info(
                "Others may join with the following command:\r\n\r\n"
                "/join " + (host == "127.0.0.1" ? "<Your IP Address>" : host) + (port == 6400 ? "" : ":" + to_string(port)) + (path == "/" ? "" : path) + "\r\n"
            );
            break;
        }

        case PING: {
            can_recv_udp |= !reliable;
            packet pong;
            pong << PONG << reliable;
            while (p.available()) {
                pong << p.read<uint8_t>();
            }
            send(pong, !can_send_udp);
            break;
        }

        case PONG: {
            can_send_udp |= !p.read<bool>();
            break;
        }

        case QUIT: {
            remove_user(p.read<uint32_t>());
            if (started) {
                on_input();
            }
            break;
        }

        case NAME: {
            auto user = user_map.at(p.read<uint32_t>());
            auto name = p.read<string>();
            my_dialog->info(user->name + " is now " + name);
            user->name = name;
            update_user_list();
            break;
        }

        case SAVE_INFO: {
            auto userId = p.read<uint32_t>();
            auto user = user_map.at(userId);

            for (int i = 0; i < me->saves.size(); i++) {
                auto save_data = p.read<save_info>();
                if (user->saves[i].sha1_data != me->saves[i].sha1_data) {
                    my_dialog->info(user->name + " updated " + me->saves[i].save_name+" to \r\nhash: " + save_data.sha1_data);
                }
                
                user->saves[i] = save_data;
            }
            
            break;
        }

        case SAVE_SYNC: {
            for (int i = 0; i < me->saves.size(); i++) {
                auto save_data = p.read<save_info>();
                auto my_save = me->saves[i];
                if (my_save.sha1_data != save_data.sha1_data) {
                    my_dialog->info("Updating Save Data to \r\nhash: " + save_data.sha1_data);

                    my_save = save_data;
                    replace_save_file(my_save);
                }

                for (auto& user : user_map) {
                    if (user->saves[i].sha1_data == save_data.sha1_data)
                        continue;
                    user->saves[i] = save_data;
                }
            }
            
            update_save_info();
            send_save_info();
            break;
        }

        case LATENCY: {
            for (auto& u : user_list) {
                u->latency = p.read<double>();
            }
            update_user_list();
            break;
        }

        case MESSAGE: {
            auto user_id = p.read<uint32_t>();
            auto message = p.read<string>();
            message_received(user_id, message);
            break;
        }

        case LAG: {
            auto lag = p.read<uint8_t>();
            auto source_id = p.read<uint32_t>();
            while (p.available()) {
                user_map.at(p.read<uint32_t>())->lag = lag;
            }
            update_user_list();
            break;
        }

        case CONTROLLERS: {
            for (auto& u : user_list) {
                for (auto& c : u->controllers) {
                    p >> c;
                }
                p >> u->map;
            }

            if (!started) {
                for (int j = 0; j < 4; j++) {
                    controllers[j].Present = 1;
                    controllers[j].RawData = 0;
                    controllers[j].Plugin = PLUGIN_NONE;
                    for (int i = 0; i < 4; i++) {
                        for (auto& u : user_list) {
                            if (u->map.get(i, j)) {
                                controllers[j].Plugin = max(controllers[j].Plugin, u->controllers[i].plugin);
                            }
                        }
                    }
                }
            }

            update_user_list();
            break;
        }

        case START: {
            start_game();
            break;
        }

        case GOLF: {
            set_golf_mode(p.read<bool>());
            break;
        }

        case INPUT_MAP: {
            auto user = user_map.at(p.read<uint32_t>());
            user->map = p.read<input_map>();
            update_user_list();
            break;
        }

        case INPUT_AUTHORITY: {
            auto user = user_map.at(p.read<uint32_t>());
            auto authority = p.read<application>();
            if (user == me) {
                set_input_authority(authority, HOST);
            } else {
                user->input_authority = authority;
            }
            update_user_list();
            break;
        }

        case INPUT_DATA: {
            while (p.available()) {
                auto user = user_map.at(p.read_var<uint32_t>());
                auto input_id = p.read_var<uint32_t>();
                auto pin = p.read_rle().transpose(input_data::SIZE, 0);
                if (!user) continue;
                while (pin.available()) {
                    auto input = pin.read<input_data>();
                    if (!user->add_input_history(input_id++, input)) continue;
                    user->input_queue.push_back(input);
                }
            }
            on_input();
            break;
        }
    }
}

void client::map_src_to_dst() {
    me->map = input_map::IDENTITY_MAP;
    for (int i = 0; i < 4; i++) {
        controllers[i].Plugin = me->controllers[i].plugin;
        controllers[i].Present = me->controllers[i].present;
        controllers[i].RawData = me->controllers[i].raw_data;
    }
}

void client::update_user_list() {
    vector<string> lines;
    lines.reserve(user_list.size());

    for (auto& u : user_list) {
        string line = "[";
        for (int j = 0; j < 4; j++) {
            int i;
            for (i = 0; i < 4 && !u->map.get(i, j); i++);

            if (i == 4) {
                line += "- ";
            } else {
                line += to_string(i + 1);
                switch (u->controllers[i].plugin) {
                    case PLUGIN_MEMPAK: line += "M"; break;
                    case PLUGIN_RUMBLE_PAK: line += "R"; break;
                    case PLUGIN_TANSFER_PAK: line += "T"; break;
                    default: line += " ";
                }
            }
        }
        line += "][" + (u->input_authority == CLIENT ? to_string(u->lag) : "H") + "] ";
        line += u->name;
        if (!isnan(u->latency)) {
            line += " (" + to_string((int)(u->latency * 1000)) + " ms)";
        }

        lines.push_back(line);
    }

    my_dialog->update_user_list(lines);
}

void client::set_input_map(input_map new_map) {
    if (me->map == new_map) return;

    me->map = new_map;
    send_input_map(new_map);

    update_user_list();
}

void client::on_tick() {
    if (can_recv_udp) return;
    send_ping();
    timer.expires_after(500ms);

    auto self(weak_from_this());
    timer.async_wait([self, this](const error_code& error) { 
        if (self.expired()) return;
        if (!error) on_tick();
    });
}

void client::send_join(const string& room) {
    if (udp_socket && udp_socket->is_open()) {
        uint16_t udp_port = udp_socket->local_endpoint().port();
        send(packet() << JOIN << PROTOCOL_VERSION << room << *me << udp_port);
    } else {
        send(packet() << JOIN << PROTOCOL_VERSION << room << *me << static_cast<uint16_t>(0));
    }
}

void client::send_name() {
    send(packet() << NAME << me->name);
}

void client::send_message(const string& message) {
    send(packet() << MESSAGE << message);
}

void client::send_save_info() {
    packet p;
    p << SAVE_INFO;
    for (auto& save : me->saves) {
        p << save;
    }
    send(p);
}

void client::send_savesync() {
    packet p;
    p << SAVE_SYNC;
    for (auto& save : me->saves) {
        p << save;
    }
    send(p);
}

void client::send_controllers() {
    packet p;
    p << CONTROLLERS;
    for (auto& c : me->controllers) {
        p << c;
    }
    send(p);
}

void client::send_start_game() {
    send(packet() << START);
}

void client::send_lag(uint8_t lag, bool my_lag, bool your_lag) {
    send(packet() << LAG << lag << my_lag << your_lag);
}

void client::send_autolag(int8_t value) {
    send(packet() << AUTOLAG << value);
}

void client::send_input(const input_data& input) {
    me->add_input_history(me->input_id, input);
    me->input_queue.push_back(input);

    if (!is_open()) return;

    packet pin;
    for (auto& e : me->input_history) {
        pin << e;
    }

    packet p;
    p << INPUT_DATA << CLIENT;
    p.write_var(me->input_id - me->input_history.size());
    p.write_rle(pin.transpose(0, input_data::SIZE));
    send(p, false);

    p.reset() << INPUT_DATA << CLIENT;
    p.write_var(me->input_id - 1);
    p.write_rle(pin.reset() << me->input_history.back());
    send(p, true);
}

void client::send_hia_input(const input_data& input) {
    send(packet() << INPUT_DATA << HOST << input, false);
}

void client::send_input_map(input_map map) {
    send(packet() << INPUT_MAP << map);
}

void client::send_ping() {
    packet p;
    p << PING << timestamp();
    send(p, false);
    if (!can_send_udp) {
        send(p, true);
    }
}
