#include "tunsession.h"

#include <ostream>
#include <string>
#include <lwipopts.h>

#include "core/service.h"
#include "core/utils.h"
#include "proto/trojanrequest.h"
#include "proto/udppacket.h"

using namespace std;

TUNSession::TUNSession(Service* _service, bool _is_udp) : 
    Session(_service, _service->config),
    m_service(_service), 
    m_recv_buf_guard(false),
    m_recv_buf_ack_length(0),
    m_out_socket(_service->get_io_context(), _service->get_ssl_context()),
    m_out_resolver(_service->get_io_context()),
    m_destroyed(false),
    m_close_from_tundev_flag(false),
    m_connected(false),
    m_udp_timout_timer(_service->get_io_context()){

    is_udp_forward_session = _is_udp;
    pipeline_com.allocate_session_id();
}

TUNSession::~TUNSession(){
    pipeline_com.free_session_id();
}

void TUNSession::start(){
    reset_udp_timeout();

    auto self = shared_from_this();
    auto cb = [this, self](){
        m_connected  = true;

        if(is_udp_forward()){
            _log_with_endpoint(m_local_addr_udp, "TUNSession session_id: " + to_string(get_session_id()) + " started ", Log::INFO);
        }else{
            _log_with_endpoint(m_local_addr, "TUNSession session_id: " + to_string(get_session_id()) + " started ", Log::INFO);
        }  

        auto insert_pwd = [this](){
            if(is_udp_forward()){
                streambuf_append(m_send_buf, TrojanRequest::generate(config.password.cbegin()->first, 
                    m_remote_addr_udp.address().to_string(), m_remote_addr_udp.port(), false));
            }else{
                streambuf_append(m_send_buf, TrojanRequest::generate(config.password.cbegin()->first, 
                    m_remote_addr.address().to_string(), m_remote_addr.port(), true));
            }
        };

        if(m_send_buf.size() > 0){
            boost::asio::streambuf tmp_buf;
            streambuf_append(tmp_buf, m_send_buf);
            m_send_buf.consume(m_send_buf.size());
            insert_pwd();
            streambuf_append(m_send_buf, tmp_buf);
        }else{
            insert_pwd();
        }

        out_async_send_impl(streambuf_to_string_view(m_send_buf), [this](boost::system::error_code ec){
            if(ec){
                output_debug_info_ec(ec);
                destroy();
                return;
            }
            out_async_read();
        });
        m_send_buf.consume(m_send_buf.size());
    };

    if(m_service->is_use_pipeline()){
        cb();
    }else{
        m_service->config.prepare_ssl_reuse(m_out_socket);
        if(is_udp_forward()){
            connect_remote_server_ssl(this, m_service->config.remote_addr, to_string(m_service->config.remote_port), 
                m_out_resolver, m_out_socket, m_local_addr_udp ,  cb);
        }else{
            connect_remote_server_ssl(this, m_service->config.remote_addr, to_string(m_service->config.remote_port), 
                m_out_resolver, m_out_socket, m_local_addr,  cb);
        }
        
    }
}

void TUNSession::reset_udp_timeout(){
    if(is_udp_forward()){
        m_udp_timout_timer.cancel();

        m_udp_timout_timer.expires_after(chrono::seconds(m_service->config.udp_timeout));
        auto self = shared_from_this();
        m_udp_timout_timer.async_wait([this, self](const boost::system::error_code error) {
            if (!error) {
                _log_with_endpoint(m_local_addr_udp, "session_id: " + to_string(get_session_id()) + " UDP TUNSession timeout", Log::INFO);
                destroy();
            }
        });
    }
}

void TUNSession::destroy(bool pipeline_call){
    if(m_destroyed){
        return;
    }
    m_destroyed = true;

    if(is_udp_forward()){
        _log_with_endpoint(m_local_addr_udp, "TUNSession session_id: " + to_string(get_session_id()) + " disconnected ", Log::INFO);
    }else{
        _log_with_endpoint(m_local_addr, "TUNSession session_id: " + to_string(get_session_id()) + " disconnected ", Log::INFO);
    }    

    m_wait_ack_handler.clear();
    m_out_resolver.cancel();   
    m_udp_timout_timer.cancel();
    shutdown_ssl_socket(this, m_out_socket);

    if(!pipeline_call && m_service->is_use_pipeline()){
        service->session_destroy_in_pipeline(*this);
    }

    if(!m_close_from_tundev_flag){
        m_close_cb(this);
    }    
}

void TUNSession::recv_ack_cmd(){
    Session::recv_ack_cmd();
    if(!m_wait_ack_handler.empty()){
        m_wait_ack_handler.front()(boost::system::error_code());
        m_wait_ack_handler.pop_front();
    }
}

void TUNSession::out_async_send_impl(const std::string_view& data_to_send, SentHandler&& _handler){
    auto self = shared_from_this();
    if(m_service->is_use_pipeline()){

        m_service->session_async_send_to_pipeline(*this, PipelineRequest::DATA, data_to_send,
         [this, self, _handler](const boost::system::error_code error) {
            reset_udp_timeout();
            if (error) {
                output_debug_info_ec(error);
                destroy();
            }else{
                if(!is_udp_forward()){
                    if(!pipeline_com.pre_call_ack_func()){
                        m_wait_ack_handler.emplace_back(move(_handler));
                        _log_with_endpoint_DEBUG(m_local_addr, "session_id: " + to_string(get_session_id()) + " cannot TUNSession::out_async_send ! Is waiting for ack");
                        return;
                    }
                    _log_with_endpoint_DEBUG(m_local_addr, "session_id: " + to_string(get_session_id()) + " permit to TUNSession::out_async_send ! ack:" + to_string(pipeline_com.pipeline_ack_counter));
                }
            }
            _handler(error);         
        });
    }else{
        auto data_copy = get_service()->get_sending_data_allocator().allocate(data_to_send);
        boost::asio::async_write(m_out_socket, data_copy->data(), [this, self, data_copy, _handler](const boost::system::error_code error, size_t) {
            get_service()->get_sending_data_allocator().free(data_copy);
            reset_udp_timeout();
            if (error) {
                output_debug_info_ec(error);
                destroy();
            }

            _handler(error);
        });
    }
}
void TUNSession::out_async_send(const uint8_t* _data, size_t _length, SentHandler&& _handler){
    if(!m_connected){
        if(m_send_buf.size() < numeric_limits<uint16_t>::max()){
            if(is_udp_forward()){
                UDPPacket::generate(m_send_buf, m_remote_addr_udp, string_view((const char*)_data, _length));
            }else{
                streambuf_append(m_send_buf, _data, _length);
            }
        }
    }else{     
        if(is_udp_forward()){
            m_send_buf.consume(m_send_buf.size());
            UDPPacket::generate(m_send_buf, m_remote_addr_udp, string_view((const char*)_data, _length));
            out_async_send_impl(streambuf_to_string_view(m_send_buf), move(_handler));
        }else{
            out_async_send_impl(string_view((const char*)_data, _length), move(_handler));
        }        
    }
}

void TUNSession::try_out_async_read(){
    if(is_destroyed()){
        return;
    }
    
    if(m_service->is_use_pipeline() && !is_udp_forward()){
        auto self = shared_from_this();
        m_service->session_async_send_to_pipeline(*this, PipelineRequest::ACK, "", [this, self](const boost::system::error_code error) {
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }

            out_async_read();
        });
    }else{
        out_async_read();
    }
    
}
void TUNSession::recv_buf_sent(uint16_t _length){
    assert(!is_udp_forward());
    m_recv_buf_ack_length -= _length;
}

void TUNSession::recv_buf_consume(uint16_t _length){
    assert(!is_udp_forward());
    m_recv_buf.consume(_length);
    if(m_recv_buf.size() == 0){
        try_out_async_read();
    }
}

size_t TUNSession::parse_udp_packet_data(const string_view& data){

    string_view parse_data(data);
    size_t parsed_size = 0;
    for(;;){
        if(parse_data.size() == 0){
            break;
        }

        // parse trojan protocol
        UDPPacket packet;
        size_t packet_len;
        if(!packet.parse(parse_data, packet_len)){
            if(parse_data.length() > numeric_limits<uint16_t>::max()){
                _log_with_endpoint(get_udp_local_endpoint(), "[tun] error UDPPacket.parse! destroy it.", Log::ERROR);
                destroy();
                break;
            }else{
                _log_with_endpoint(get_udp_local_endpoint(), "[tun] error UDPPacket.parse! Might need to read more...", Log::WARN);
            }
            break;
        }

        if(m_write_to_lwip(this, &packet.payload) < 0){
            output_debug_info();
            destroy();
            break;
        }

        parsed_size += packet_len;
        parse_data = parse_data.substr(packet_len);
    }

    return parsed_size;
}

void TUNSession::out_async_read() {
    if(m_service->is_use_pipeline()){    
        get_pipeline_component().pipeline_data_cache.async_read([this](const string_view &data) {
            if(is_udp_forward()){
                
                reset_udp_timeout();

                if(m_recv_buf.size() == 0){
                    auto parsed = parse_udp_packet_data(data);
                    if(parsed < data.length()){
                        streambuf_append(m_recv_buf, data.substr(parsed));
                    }
                }else{
                    streambuf_append(m_recv_buf, data);
                    auto parsed = parse_udp_packet_data(streambuf_to_string_view(m_recv_buf));
                    m_recv_buf.consume(parsed);
                }                

                try_out_async_read();
            }else{
                streambuf_append(m_recv_buf, data);
                m_recv_buf_ack_length += data.length();

                if(m_write_to_lwip(this, nullptr) < 0){
                    output_debug_info();
                    destroy();
                }
            }
            
        });
    }else{
        _guard_read_buf_begin(m_recv_buf);
        auto self = shared_from_this();
        m_out_socket.async_read_some(m_recv_buf.prepare(Session::MAX_BUF_LENGTH), [this, self](const boost::system::error_code error, size_t length) {
            _guard_read_buf_end(m_recv_buf);
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }
            m_recv_buf.commit(length);

            if(is_udp_forward()){
                reset_udp_timeout();    
                auto parsed = parse_udp_packet_data(streambuf_to_string_view(m_recv_buf));
                m_recv_buf.consume(parsed);

                try_out_async_read();
            }else{
                m_recv_buf_ack_length += length;

                if(m_write_to_lwip(this, nullptr) < 0){
                    output_debug_info();
                    destroy();
                }
            }            
        });
    }
}

bool TUNSession::try_to_process_udp(const boost::asio::ip::udp::endpoint& _local, 
        const boost::asio::ip::udp::endpoint& _remote, const uint8_t* payload, size_t payload_length){
            
    if(is_udp_forward()){
        if(_local == m_local_addr_udp && _remote == m_remote_addr_udp){
            out_async_send(payload, payload_length, [](boost::system::error_code){});
            return true;
        }
    }

    return false;
}
