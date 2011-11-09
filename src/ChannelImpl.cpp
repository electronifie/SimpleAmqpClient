#include "SimpleAmqpClient/ChannelImpl.h"
#include "SimpleAmqpClient/AmqpResponseLibraryException.h"
#include "SimpleAmqpClient/AmqpResponseServerException.h"
#include "SimpleAmqpClient/ConsumerTagNotFoundException.h"

#include "config.h"

#include <boost/array.hpp>

// This will get us the posix version of strerror_r() on linux
#ifndef _XOPEN_SOURCE
# define _XOPEN_SOURCE 600
#endif
#include <string.h>
#include <sys/types.h>
#include <errno.h>

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
# include <sys/select.h>
#endif

#ifdef HAVE_WINSOCK2_H
# define NOMINMAX
# include <WinSock2.h>
#endif

namespace AmqpClient
{
namespace Detail
{

ChannelImpl::ChannelImpl() :
  m_next_channel_id(1)
{
  // Channel 0 is always open
  m_open_channels.insert(std::make_pair(0, frame_queue_t()));
}

ChannelImpl::~ChannelImpl()
{
}

amqp_channel_t ChannelImpl::GetNextChannelId()
{
  int max_channels = amqp_get_channel_max(m_connection);
  int channel_count = m_open_channels.size();
  if (0 == max_channels)
  {
    if (std::numeric_limits<uint16_t>::max() <= channel_count)
    {
      throw std::runtime_error("Too many channels open");
    }
  }
  else if (max_channels <= channel_count)
  {
    throw std::runtime_error("Too many channels open");
  }

  while (m_open_channels.end() != m_open_channels.find(++m_next_channel_id)) { /* Empty */ }

  m_open_channels.insert(std::make_pair(m_next_channel_id, frame_queue_t()));
  return m_next_channel_id;
}

amqp_channel_t ChannelImpl::CreateNewChannel()
{
  amqp_channel_t new_channel = GetNextChannelId();

  static const boost::array<uint32_t, 1> OPEN_OK = { { AMQP_CHANNEL_OPEN_OK_METHOD } };
  amqp_channel_open_t channel_open = { 0 /* Out of band = false */ };
  DoRpcOnChannel<boost::array<uint32_t, 1> >(new_channel, AMQP_CHANNEL_OPEN_METHOD, &channel_open, OPEN_OK);

  static const boost::array<uint32_t, 1> CONFIRM_OK = { { AMQP_CONFIRM_SELECT_OK_METHOD } };
  amqp_confirm_select_t confirm_select = { 0 /* nowait = false */ };
  DoRpcOnChannel<boost::array<uint32_t, 1> >(new_channel, AMQP_CONFIRM_SELECT_METHOD, &confirm_select, CONFIRM_OK);

  return new_channel;
}

amqp_channel_t ChannelImpl::GetChannel()
{
  if (m_free_channels.empty())
  {
    return CreateNewChannel();
  }
  else
  {
    amqp_channel_t ret = m_free_channels.front();
    m_free_channels.pop();
    return ret;
  }
}

void ChannelImpl::ReturnChannel(amqp_channel_t channel)
{
  m_free_channels.push(channel);
  amqp_maybe_release_buffers(m_connection);
}


bool ChannelImpl::IsChannelOpen(amqp_channel_t channel)
{
  return m_open_channels.find(channel) != m_open_channels.end();
}


void ChannelImpl::FinishCloseChannel(amqp_channel_t channel)
{
  amqp_channel_close_ok_t close_ok;
  m_open_channels.erase(channel);
  CheckForError(amqp_send_method(m_connection, channel, AMQP_CHANNEL_CLOSE_OK_METHOD, &close_ok), "ChannelImpl::FinishCloseChannel channel.close");
}

void ChannelImpl::FinishCloseConnection()
{
  amqp_connection_close_ok_t close_ok;
  amqp_send_method(m_connection, 0, AMQP_CONNECTION_CLOSE_OK_METHOD, &close_ok);
}

void ChannelImpl::CheckRpcReply(amqp_channel_t channel, const amqp_rpc_reply_t& reply, const std::string& context)
{
  switch (reply.reply_type)
  {
  case AMQP_RESPONSE_NORMAL:
    return;
    break;

  case AMQP_RESPONSE_NONE:
    throw std::logic_error("Got a amqp_rpc_reply_t with no reply_type!");
    break;

  case AMQP_RESPONSE_LIBRARY_EXCEPTION:
    // If we're getting this likely is the socket is already closed
    throw AmqpResponseLibraryException(reply, context);

    break;

  case AMQP_RESPONSE_SERVER_EXCEPTION:
    if (reply.reply.id == AMQP_CHANNEL_CLOSE_METHOD) 
    {
      FinishCloseChannel(channel);
    }
    else if (reply.reply.id == AMQP_CONNECTION_CLOSE_METHOD)
    {
      FinishCloseConnection();
    }

    throw AmqpResponseServerException(reply, context);
    break;
  default:
    throw std::runtime_error("amqp_rpc_reply_t that didn't match!");
  }
}

void ChannelImpl::CheckForError(int ret, const std::string& context)
{
  if (ret < 0)
  {
    char* errstr = amqp_error_string(-ret);
    std::ostringstream oss;
    oss << context << ": " << errstr;
    free(errstr);
    throw std::runtime_error(oss.str().c_str());
  }
}

MessageReturnedException ChannelImpl::CreateMessageReturnedException(amqp_basic_return_t& return_method, amqp_channel_t channel)
{
  const int reply_code = return_method.reply_code;
  const std::string reply_text((char*)return_method.reply_text.bytes, return_method.reply_text.len);
  const std::string exchange((char*)return_method.exchange.bytes, return_method.exchange.len);
  const std::string routing_key((char*)return_method.routing_key.bytes, return_method.routing_key.len);
  BasicMessage::ptr_t content = ReadContent(channel);
  return MessageReturnedException(content, reply_code, reply_text, exchange, routing_key);
}

BasicMessage::ptr_t ChannelImpl::ReadContent(amqp_channel_t channel)
{
  amqp_frame_t frame;

  GetNextFrameOnChannel(channel, frame);

  if (frame.frame_type != AMQP_FRAME_HEADER)
    // TODO: We should connection.close here
    throw std::runtime_error("Channel::BasicConsumeMessage: receieved unexpected frame type (was expected AMQP_FRAME_HEADER)");

  // The memory for this is allocated in a pool associated with the connection
  // The BasicMessage constructor does a deep copy of the properties structure
  amqp_basic_properties_t* properties = reinterpret_cast<amqp_basic_properties_t*>(frame.payload.properties.decoded);

  size_t body_size = frame.payload.properties.body_size;
  size_t received_size = 0;
  amqp_bytes_t body = amqp_bytes_malloc(body_size);

  // frame #3 and up:
  while (received_size < body_size)
  {
    GetNextFrameOnChannel(channel, frame);

    if (frame.frame_type != AMQP_FRAME_BODY)
      // TODO: we should connection.close here
      throw std::runtime_error("Channel::BasicConsumeMessge: received unexpected frame type (was expecting AMQP_FRAME_BODY)");

    void* body_ptr = reinterpret_cast<char*>(body.bytes) + received_size;
    memcpy(body_ptr, frame.payload.body_fragment.bytes, frame.payload.body_fragment.len);
    received_size += frame.payload.body_fragment.len;
  }
  return BasicMessage::Create(body, properties);
}

void ChannelImpl::CheckFrameForClose(amqp_frame_t& frame, amqp_channel_t channel)
{
  if (frame.frame_type == AMQP_FRAME_METHOD)
  {
    switch (frame.payload.method.id)
    {
    case AMQP_CHANNEL_CLOSE_METHOD:
      FinishCloseChannel(channel);
      throw AmqpResponseServerException(*reinterpret_cast<amqp_channel_close_t*>(frame.payload.method.decoded), "Consuming message");
      break;

    case AMQP_CONNECTION_CLOSE_METHOD:
      FinishCloseConnection();
      throw AmqpResponseServerException(*reinterpret_cast<amqp_connection_close_t*>(frame.payload.method.decoded), "Consuming message");
      break;
    }
  }
}

void ChannelImpl::AddConsumer(const std::string& consumer_tag, amqp_channel_t channel)
{
  m_consumer_channel_map.insert(std::make_pair(consumer_tag, channel));
}

amqp_channel_t ChannelImpl::RemoveConsumer(const std::string& consumer_tag)
{
  std::map<std::string, amqp_channel_t>::iterator it = m_consumer_channel_map.find(consumer_tag);
  if (it == m_consumer_channel_map.end())
  {
    throw ConsumerTagNotFoundException();
  }

  amqp_channel_t result = it->second;

  m_consumer_channel_map.erase(it);

  return result;
}

amqp_channel_t ChannelImpl::GetConsumerChannel(const std::string& consumer_tag)
{
  std::map<std::string, amqp_channel_t>::const_iterator it = m_consumer_channel_map.find(consumer_tag);
  if (it == m_consumer_channel_map.end())
  {
    throw ConsumerTagNotFoundException();
  }
  return it->second;
}

ChannelImpl::channel_map_iterator_t ChannelImpl::GetChannelQueueOrThrow(amqp_channel_t channel)
{
  channel_map_iterator_t ret = m_open_channels.find(channel);
  if (m_open_channels.end() == ret)
  {
    throw std::runtime_error("Channel not found");
  }
  return ret;
}

bool ChannelImpl::GetNextFrameFromBroker(amqp_frame_t& frame, boost::chrono::microseconds timeout)
{
  int socketno = amqp_get_sockfd(m_connection);

start:
  // Possibly set a timeout on receiving
  if (timeout != boost::chrono::microseconds::max())
  {
    struct timeval tv_timeout;
    memset(&tv_timeout, 0, sizeof(tv_timeout));
    tv_timeout.tv_usec = timeout.count();

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socketno, &fds);

    int select_return = select(socketno + 1, &fds, NULL, NULL, &tv_timeout);

    if (select_return == 0) // If it times out, return
    {
      return false;
    }
    else if (select_return == -1)
    {
      // If its an interupted system call just try again
      if (errno == EINTR)
      {
        goto start;
      }
      else
      {
        std::string error_string("error calling select on socket: ");
#ifdef HAVE_STRERROR_S
        const int BUFFER_LENGTH = 256;
        char error_string_buffer[BUFFER_LENGTH] = {0};
        strerror_s(error_string_buffer, errno);
        error_string += error_string_buffer;
#elif defined(HAVE_STRERROR_R)
        const int BUFFER_LENGTH = 256;
        char error_string_buffer[BUFFER_LENGTH] = {0};
        strerror_r(errno, error_string_buffer, BUFFER_LENGTH);
        error_string += error_string_buffer;
#else
        error_string += strerror(errno);
#endif
        throw std::runtime_error(error_string.c_str());
      }
    }
  }

  CheckForError(amqp_simple_wait_frame(m_connection, &frame), "ChannelImpl::GetNextFrameOnChannel");
  return true;
}

bool ChannelImpl::GetNextFrameFromBrokerOnChannel(amqp_channel_t channel, amqp_frame_t& frame, boost::chrono::microseconds timeout)
{
  boost::chrono::steady_clock::time_point end_point;
  boost::chrono::microseconds timeout_left = timeout;
  if (timeout != boost::chrono::microseconds::max())
  {
    end_point = boost::chrono::steady_clock::now() + timeout;
  }

  amqp_frame_t received_frame;
  while (GetNextFrameFromBroker(received_frame, timeout_left))
  {
    if (frame.channel == channel)
    {
      frame = received_frame;
      return true;
    }

    if (frame.channel == 0)
    {
      // Only thing we care to handle on the channel0 is the connection.close method
      if (AMQP_FRAME_METHOD == frame.frame_type &&
        AMQP_CONNECTION_CLOSE_METHOD == frame.payload.method.id)
      {
        FinishCloseConnection();
        throw AmqpResponseServerException(*reinterpret_cast<amqp_connection_close_t*>(frame.payload.method.decoded), "ChannelImpl::GetNextFrameFromBrokerOnChannel");
      }
    }
    else
    {
      GetChannelQueueOrThrow(frame.channel)->second.push_back(received_frame);
    }

    if (timeout != boost::chrono::microseconds::max())
    {
      boost::chrono::steady_clock::time_point now = boost::chrono::steady_clock::now();
      if (now >= end_point)
      {
        return false;
      }
      timeout_left = boost::chrono::duration_cast<boost::chrono::microseconds>(end_point - now);
    }
  }
  return false;
}

bool ChannelImpl::GetNextFrameOnChannel(amqp_channel_t channel, amqp_frame_t& frame, boost::chrono::microseconds timeout)
{
  frame_queue_t channel_queue = GetChannelQueueOrThrow(channel)->second;

  if (!channel_queue.empty())
  {
    frame_queue_t::iterator it = channel_queue.begin();
    frame = *it;
    channel_queue.erase(it);

    if (AMQP_FRAME_METHOD == frame.frame_type &&
      AMQP_CHANNEL_CLOSE_METHOD == frame.payload.method.id)
    {
      FinishCloseChannel(channel);
      throw AmqpResponseServerException(*reinterpret_cast<amqp_channel_close_t*>(frame.payload.method.decoded), "ChannelImpl::GetNextFrameOnChannel");
    }
    return true;
  }

  return GetNextFrameFromBrokerOnChannel(channel, frame, timeout);
}
} // namespace Detail
} // namespace AmqpClient