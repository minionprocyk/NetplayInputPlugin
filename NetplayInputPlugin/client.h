#pragma once

#include "stdafx.h"

#include "connection.h"
#include "packet.h"
#include "Controller 1.1.h"
#include "common.h"
#include "client_dialog.h"
#include "server.h"

int get_input_rate(country_code code);

class client: public service_wrapper, public connection {
    public:
        client(std::shared_ptr<client_dialog> dialog);
        ~client();
        void add_server(std::string& server);
        void remove_server(std::string& server);
        void load_public_server_list();
        void ping_public_server_list();
        std::string get_name();
        std::string get_favorite_server();
        void set_name(const std::string& name);
        void set_rom_info(const rom_info& rom);
        void set_save_info(const std::string& save_path);
        void set_favorite_server(const std::string& fav_server);
        void set_src_controllers(CONTROL controllers[4]);
        void set_dst_controllers(CONTROL controllers[4]);
        void process_input(std::array<BUTTONS, 4>& input);
        void wait_until_start();
        void post_close();
        virtual void on_receive(packet& packet, bool reliable);
        virtual void on_error(const std::error_code& error);
    private:
        asio::ip::udp::resolver udp_resolver;
        asio::steady_timer timer;
        bool started = false;
        std::mutex start_mutex;
        std::condition_variable start_condition;
        std::mutex next_input_mutex;
        std::condition_variable next_input_condition;
        std::list<std::array<BUTTONS, 4>> next_input;
        uint32_t input_id = 0;
        bool golf = false;
        input_data pending_cia_input;
        std::string host;
        uint16_t port;
        std::string path;
        std::string save_path;
        std::shared_ptr<user_info> me = std::make_shared<user_info>();
        std::vector<std::shared_ptr<user_info>> user_map = { me };
        std::vector<std::shared_ptr<user_info>> user_list = { me };
        std::map<std::string, double> public_servers;
        CONTROL* controllers;
        std::shared_ptr<client_dialog> my_dialog;
        std::shared_ptr<server> my_server;
        bool frame_limit = true;
        
#ifdef DEBUG
        std::ofstream input_log;
#endif

        virtual void close();
        void start_game();
        void on_message(std::string message);
        void set_lag(uint8_t lag);
        void message_received(uint32_t id, const std::string& message);
        void remove_user(uint32_t id);
        void connect(const std::string& host, uint16_t port, const std::string& room);
        void map_src_to_dst();
        void on_input();
        void on_tick();
        void update_user_list();
        void set_input_map(input_map map);
        void set_input_authority(application authority, application initiator = CLIENT);
        void set_golf_mode(bool golf);
        void send_join(const std::string& room);
        void send_name();
        void send_save_info();
        void send_controllers();
        void send_message(const std::string& message);
        void send_start_game();
        void send_lag(uint8_t lag, bool my_lag, bool your_lag);
        void send_input(const input_data& input);
        void send_hia_input(const input_data& input);
        void send_autolag(int8_t value = -1);
        void send_input_map(input_map map);
        void send_ping(); 
        void send_savesync();
        void update_save_info();
        std::vector<std::string> find_rom_save_files(const std::string& path);
        std::string sha1_save_info(const save_info& saveInfo);
        std::string slurp(const std::string& file);
        std::string slurp2(const std::string& file);
        void replace_save_file(const save_info& save_data);

};
