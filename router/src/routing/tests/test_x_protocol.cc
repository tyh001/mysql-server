/*
  Copyright (c) 2016, 2020, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <memory>
#include <system_error>

#include <gmock/gmock.h>
#include <google/protobuf/io/coded_stream.h>
#include <gtest/gtest.h>

#include "mysql/harness/logging/logging.h"
#include "mysql/harness/stdx/expected.h"
#include "mysqlrouter/routing.h"
#include "mysqlx.pb.h"
#include "mysqlx_connection.pb.h"
#include "mysqlx_notice.pb.h"
#include "mysqlx_session.pb.h"
#include "protocol/x_protocol.h"
#include "routing_mocks.h"
#include "test/helpers.h"

using ::testing::_;
using ::testing::Return;

class XProtocolTest : public ::testing::Test {
 protected:
  XProtocolTest() : x_protocol_(new XProtocol(&mock_sock_ops_)) {}

  void SetUp() override {
    network_buffer_.resize(routing::kDefaultNetBufferLength);
    network_buffer_offset_ = 0;
    curr_pktnr_ = 0;
    handshake_done_ = false;
  }

  MockSocketOperations mock_sock_ops_;
  std::unique_ptr<BaseProtocol> x_protocol_;

  void serialize_protobuf_msg_to_buffer(RoutingProtocolBuffer &buffer,
                                        size_t &buffer_offset,
                                        google::protobuf::MessageLite &msg,
                                        unsigned char type) {
    size_t msg_size = message_byte_size(msg);
    google::protobuf::io::CodedOutputStream::WriteLittleEndian32ToArray(
        static_cast<uint32_t>(msg_size + 1), &buffer[buffer_offset]);
    buffer[buffer_offset + 4] = type;
    bool res = msg.SerializeToArray(&buffer[buffer_offset + 5],
                                    message_byte_size(msg));
    buffer_offset += (msg_size + 5);
    ASSERT_TRUE(res);
  }

  static constexpr int sender_socket_ = 1;
  static constexpr int receiver_socket_ = 2;

  RoutingProtocolBuffer network_buffer_;
  size_t network_buffer_offset_;
  int curr_pktnr_;
  bool handshake_done_;
};

static Mysqlx::Session::AuthenticateStart create_authenticate_start_msg() {
  Mysqlx::Session::AuthenticateStart result;
  result.set_mech_name("PLAIN");

  return result;
}

Mysqlx::Error create_error_msg(unsigned short code, const std::string &message,
                               const std::string &sql_state) {
  Mysqlx::Error result;
  result.set_code(code);
  result.set_sql_state(sql_state);
  result.set_msg(message);

  return result;
}

Mysqlx::Notice::Warning create_warning_msg(unsigned int code,
                                           const std::string &message) {
  Mysqlx::Notice::Warning result;
  result.set_code(code);
  result.set_msg(message);

  return result;
}

static Mysqlx::Connection::CapabilitiesSet create_capab_set_msg() {
  Mysqlx::Connection::CapabilitiesSet result;

  Mysqlx::Connection::Capability *capability =
      result.mutable_capabilities()->add_capabilities();
  capability->set_name("tls");
  capability->mutable_value()->set_type(Mysqlx::Datatypes::Any_Type_SCALAR);
  capability->mutable_value()->mutable_scalar()->set_type(
      Mysqlx::Datatypes::Scalar_Type_V_UINT);
  capability->mutable_value()->mutable_scalar()->set_v_unsigned_int(1);

  return result;
}

TEST_F(XProtocolTest, OnBlockClientHostSuccess) {
  // we expect the router sending CapabilitiesGet message
  // to prevent MySQL server from bumping up connection error counter
  const size_t msg_size =
      message_byte_size(Mysqlx::Connection::CapabilitiesGet()) + 5;

  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, _, msg_size))
      .WillOnce(Return(msg_size));

  const bool result =
      x_protocol_->on_block_client_host(receiver_socket_, "routing");

  ASSERT_TRUE(result);
}

TEST_F(XProtocolTest, OnBlockClientHostWriteFail) {
  // we expect the router sending CapabilitiesGet message
  // to prevent MySQL server from bumping up connection error counter
  const size_t msg_size =
      message_byte_size(Mysqlx::Connection::CapabilitiesGet()) + 5;

  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, _, msg_size))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::connection_refused))));

  const bool result =
      x_protocol_->on_block_client_host(receiver_socket_, "routing");

  ASSERT_FALSE(result);
}

TEST_F(XProtocolTest, CopyPacketsNoData) {
  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, false, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_TRUE(copy_res);
  EXPECT_EQ(copy_res.value(), 0);
  EXPECT_FALSE(handshake_done_);
}

TEST_F(XProtocolTest, CopyPacketsReadError) {
  EXPECT_CALL(mock_sock_ops_, read(sender_socket_, _, _))
      .WillOnce(Return(
          stdx::make_unexpected(make_error_code(std::errc::connection_reset))));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_FALSE(copy_res);
  EXPECT_FALSE(handshake_done_);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeDoneOK) {
  handshake_done_ = true;
  constexpr int MSG_SIZE = 20;

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(MSG_SIZE));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0], 20))
      .WillOnce(Return(MSG_SIZE));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  EXPECT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
  EXPECT_EQ(static_cast<size_t>(MSG_SIZE), copy_res.value());
}

TEST_F(XProtocolTest, CopyPacketsHandshakeDoneWriteError) {
  handshake_done_ = true;
  constexpr ssize_t MSG_SIZE = 20;

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(MSG_SIZE));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0], 20))
      .WillOnce(Return(stdx::make_unexpected(
          make_error_code(std::errc::connection_refused))));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_FALSE(copy_res);
  ASSERT_TRUE(handshake_done_);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsIvalidData) {
  constexpr size_t INVALID_DATA_SIZE = 20;

  // prepare some invalid data
  for (size_t i = 0; i < INVALID_DATA_SIZE; ++i) {
    network_buffer_[i] = static_cast<uint8_t>(i + 10);
  }
  network_buffer_offset_ += INVALID_DATA_SIZE;

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_FALSE(copy_res);
  EXPECT_FALSE(handshake_done_);
  EXPECT_EQ(network_buffer_offset_, INVALID_DATA_SIZE);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsWrongMessage) {
  Mysqlx::Session::Close close_msg{};

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   close_msg,
                                   Mysqlx::ClientMessages::SESS_CLOSE);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_FALSE(handshake_done_);
  ASSERT_FALSE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsAuthStart) {
  auto auth_msg = create_authenticate_start_msg();

  serialize_protobuf_msg_to_buffer(
      network_buffer_, network_buffer_offset_, auth_msg,
      Mysqlx::ClientMessages::SESS_AUTHENTICATE_START);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_TRUE(copy_res);
  ASSERT_TRUE(handshake_done_);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsCapabilitiesGet) {
  Mysqlx::Connection::CapabilitiesGet capab_msg{};

  serialize_protobuf_msg_to_buffer(
      network_buffer_, network_buffer_offset_, capab_msg,
      Mysqlx::ClientMessages::CON_CAPABILITIES_GET);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsConnectionClose) {
  Mysqlx::Connection::Close close_msg{};

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   close_msg,
                                   Mysqlx::ClientMessages::CON_CLOSE);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsCapabilitiesSet) {
  auto capab_msg = create_capab_set_msg();

  serialize_protobuf_msg_to_buffer(
      network_buffer_, network_buffer_offset_, capab_msg,
      Mysqlx::ClientMessages::CON_CAPABILITIES_SET);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeClientSendsBrokenMessage) {
  auto capab_msg = create_capab_set_msg();

  serialize_protobuf_msg_to_buffer(
      network_buffer_, network_buffer_offset_, capab_msg,
      Mysqlx::ClientMessages::CON_CAPABILITIES_SET);

  // let's brake some part of the message in the buffer to simulate malformed
  // message
  network_buffer_[6] = 0xff;

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  ASSERT_FALSE(handshake_done_);
  ASSERT_FALSE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeServerSendsError) {
  auto error_msg = create_error_msg(100, "Error message", "HY007");

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   error_msg, Mysqlx::ServerMessages::ERROR);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeServerSendsOtherMessage) {
  auto warn_msg = create_warning_msg(10023, "Warning message");

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   warn_msg, Mysqlx::ServerMessages::NOTICE);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_FALSE(handshake_done_);
  ASSERT_TRUE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeReadTwoMessages) {
  auto warn_msg = create_warning_msg(10023, "Warning message");
  auto error_msg = create_error_msg(100, "Error message", "HY007");

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   warn_msg, Mysqlx::ServerMessages::NOTICE);
  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   error_msg, Mysqlx::ServerMessages::ERROR);

  EXPECT_CALL(mock_sock_ops_,
              read(sender_socket_, &network_buffer_[0], network_buffer_.size()))
      .WillOnce(Return(network_buffer_offset_));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  // handshake_done_ should be set after the second message
  EXPECT_TRUE(handshake_done_);
  ASSERT_TRUE(copy_res);
  EXPECT_EQ(network_buffer_offset_, copy_res.value());
}

TEST_F(XProtocolTest, CopyPacketsHandshakeReadPartialHeader) {
  Mysqlx::Connection::CapabilitiesGet capab_msg{};

  serialize_protobuf_msg_to_buffer(
      network_buffer_, network_buffer_offset_, capab_msg,
      Mysqlx::ClientMessages::CON_CAPABILITIES_GET);

  EXPECT_CALL(mock_sock_ops_, read(sender_socket_, _, _))
      .Times(2)
      .WillOnce(Return(network_buffer_offset_ - 3))
      .WillOnce(Return(3));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, false);

  // handshake_done_ should bet set
  ASSERT_TRUE(copy_res);
  EXPECT_TRUE(handshake_done_);
  EXPECT_EQ(network_buffer_offset_, copy_res.value());
}

TEST_F(XProtocolTest, CopyPacketsHandshakeReadPartialMessage) {
  auto warn_msg = create_warning_msg(100, "Warning message");

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   warn_msg, Mysqlx::ServerMessages::NOTICE);

  EXPECT_CALL(mock_sock_ops_, read(sender_socket_, _, _))
      .Times(2)
      .WillOnce(Return(network_buffer_offset_ - 8))
      .WillOnce(Return(8));
  EXPECT_CALL(mock_sock_ops_, write(receiver_socket_, &network_buffer_[0],
                                    network_buffer_offset_))
      .WillOnce(Return(network_buffer_offset_));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_TRUE(copy_res);
  EXPECT_FALSE(handshake_done_);
  EXPECT_EQ(network_buffer_offset_, copy_res.value());
}

TEST_F(XProtocolTest, CopyPacketsHandshakeReadPartialMessageFails) {
  auto warn_msg = create_warning_msg(100, "Warning message");

  serialize_protobuf_msg_to_buffer(network_buffer_, network_buffer_offset_,
                                   warn_msg, Mysqlx::ServerMessages::NOTICE);

  EXPECT_CALL(mock_sock_ops_, read(sender_socket_, _, _))
      .Times(2)
      .WillOnce(Return(network_buffer_offset_ - 8))
      .WillOnce(Return(
          stdx::make_unexpected(make_error_code(std::errc::connection_reset))));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  ASSERT_FALSE(handshake_done_);
  ASSERT_FALSE(copy_res);
}

TEST_F(XProtocolTest, CopyPacketsHandshakeMsgBiggerThanBuffer) {
  Mysqlx::Connection::CapabilitiesSet capabilites_msg;
  // make the message bigger than the current network buffer size
  std::string msg;
  while (msg.size() <= routing::kDefaultNetBufferLength) {
    msg += std::string(1000, 'a');
  }
  auto error_msg = create_error_msg(100, msg, "HY007");
  const auto error_msg_size = message_byte_size(error_msg);
  assert(error_msg_size >
         static_cast<size_t>(routing::kDefaultNetBufferLength));

  RoutingProtocolBuffer msg_buffer(error_msg_size + 5);
  const auto BUFFER_SIZE = network_buffer_.size();

  serialize_protobuf_msg_to_buffer(msg_buffer, network_buffer_offset_,
                                   error_msg, Mysqlx::ServerMessages::ERROR);

  // copy part of the message to the network buffer
  std::copy(msg_buffer.begin(), msg_buffer.begin() + network_buffer_.size(),
            network_buffer_.begin());

  EXPECT_CALL(mock_sock_ops_, read(sender_socket_, _, _))
      .Times(1)
      .WillOnce(Return(network_buffer_.size()));

  const auto copy_res = x_protocol_->copy_packets(
      sender_socket_, receiver_socket_, true, network_buffer_, &curr_pktnr_,
      handshake_done_, true);

  // the size of buffer passed to copy_packets should be untouched
  ASSERT_EQ(BUFFER_SIZE, network_buffer_.size());
  ASSERT_FALSE(handshake_done_);
  ASSERT_FALSE(copy_res);
}

TEST_F(XProtocolTest, SendErrorOKMultipleWrites) {
  EXPECT_CALL(mock_sock_ops_, write(1, _, _))
      .Times(2)
      .WillOnce(Return(8))
      .WillOnce(Return(10000));

  bool res = x_protocol_->send_error(1, 55, "Error message", "SQL_STATE",
                                     "routing configuration name");

  ASSERT_TRUE(res);
}

TEST_F(XProtocolTest, SendErrorWriteFail) {
  EXPECT_CALL(mock_sock_ops_, write(1, _, _))
      .WillOnce(Return(
          stdx::make_unexpected(make_error_code(std::errc::connection_reset))));

  bool res = x_protocol_->send_error(1, 55, "Error message", "SQL_STATE",
                                     "routing configuration name");

  ASSERT_FALSE(res);
}

int main(int argc, char *argv[]) {
  init_test_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
