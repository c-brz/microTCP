/*
 * microtcp, a lightweight implementation of TCP for teaching,
 * and academic purposes.
 *
 * Copyright (C) 2015-2017  Manolis Surligas <surligas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "microtcp.h"
#include "../utils/crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <netinet/in.h>

microtcp_sock_t
microtcp_socket (int domain, int type, int protocol)
{
  microtcp_sock_t s;
  if ((s.sd = socket(domain, SOCK_DGRAM, IPPROTO_UDP)) == -1){
    perror("opening socket");
    s.state = INVALID;
    return s;
  }
  
  s.packets_send = 0;
  s.packets_received = 0;
  s.packets_lost = 0;
  s.bytes_send = 0;
  s.bytes_received = 0;
  s.bytes_lost = 0;
  
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = MICROTCP_ACK_TIMEOUT_US;

  s.state = UNKNOWN;
  return s;
}

int
microtcp_bind (microtcp_sock_t *socket, const struct sockaddr *address,
               socklen_t address_len)
{ 
  int rv;
  if((rv = bind(socket->sd, address, address_len)) == -1){
    perror("TCP bind");
  }
  
  return rv;
}

static uint16_t set_bit (uint16_t data, uint16_t pos)
{
  return (data|(1 << pos));
}

//returns 0 if bit not set, else !=0
static uint16_t get_bit (uint16_t data, uint16_t pos)
{
  return ((data >> pos) & 1);
}

static microtcp_header_t make_header (uint32_t seq_number,uint32_t ack_number, 
                                 uint16_t window, uint32_t data_len,
                                 uint8_t ACK, uint8_t RST, uint8_t SYN, uint8_t FIN)
{
  microtcp_header_t header;

  header.seq_number = htonl(seq_number);
  header.ack_number = htonl(ack_number);
  header.window = htons(window);
  header.data_len = htonl(data_len);
  header.future_use0 = 0;
  header.future_use1 = 0;
  header.future_use2 = 0;
  header.checksum = 0;
  uint16_t tmp_control = 0;
  if(ACK) set_bit(tmp_control, ACK_F);
  if(RST) set_bit(tmp_control, RST_F);
  if(SYN) set_bit(tmp_control, SYN_F);
  if(FIN) set_bit(tmp_control, FIN_F);
  header.control = htons(tmp_control);
  header.checksum = htonl(crc32((uint8_t *)(&header), sizeof(header)));

  return header;
}

//returns the given header in host byte order
static microtcp_header_t get_hbo_header (microtcp_header_t *nbo_header)
{
  microtcp_header_t hbo_header; 
  
  hbo_header.seq_number = ntohl(nbo_header->seq_number);
  hbo_header.ack_number = ntohl(nbo_header->ack_number);
  hbo_header.control = ntohs(nbo_header->control);
  hbo_header.window = ntohs(nbo_header->window);
  hbo_header.data_len = ntohl(nbo_header->data_len);
  hbo_header.future_use0 = ntohl(nbo_header->future_use0);
  hbo_header.future_use1 = ntohl(nbo_header->future_use1);
  hbo_header.future_use2 = ntohl(nbo_header->future_use2);
  hbo_header.checksum = ntohl(nbo_header->checksum);

  return hbo_header;
}

//returns 1 if header control is valid according to the given values, 0 otherwise
static int is_header_control_valid (microtcp_header_t *hbo_header, uint8_t ACK, uint8_t RST, uint8_t SYN, uint8_t FIN)
{
  if(ACK && get_bit(hbo_header->control, ACK_F) == 0)
    return 0;
  if(RST && get_bit(hbo_header->control, RST_F) == 0)
    return 0;
  if(SYN && get_bit(hbo_header->control, SYN_F) == 0)
    return 0;
  if(FIN && get_bit(hbo_header->control, FIN_F) == 0)
    return 0;

  return 1;
}

static int is_equal_addresses (const struct sockaddr a, const struct sockaddr b)
{
  if (a.sa_family != b.sa_family)
    return 0;
  if (strcmp(a.sa_data, b.sa_data) != 0)
    return 0;
  else return 1;
}


/* Calculates checksum of header recv_header
   Returns 1 if checksum calculated is equal to 
   checksum field of header else returns 0 */

static int is_checksum_valid(const uint8_t *recv_buf, const size_t msg_len){

  microtcp_header_t *tmp_header;
  uint32_t received_checksum, calculated_checksum;
  int i = 0, size = 0;

  tmp_header = malloc(msg_len);
  
  memcpy(tmp_header, recv_buf, size);

  /* check sum in received header */
  received_checksum = ntohl(tmp_header->checksum);

  /* calculate checksum of header for comparison */
  calculated_checksum = ntohl(crc32(recv_buf, msg_len));

  return (received_checksum == calculated_checksum);
}


int
microtcp_connect (microtcp_sock_t *socket, const struct sockaddr *address,
                  socklen_t address_len)
{
  microtcp_header_t syn, synack, ack;
  struct sockaddr src_addr;
  socklen_t src_addr_length;
  ssize_t bytes_sent, ret;
  char tmp_buf[MICROTCP_RECVBUF_LEN];

  srand(time(NULL));
  socket->seq_number = rand();  // create random sequence number

  /* create the header for the 1st step of the 3-way handshake (SYN segment) */
  syn = make_header(socket->seq_number, 0, 0, 0, 0, 0, 1, 0);
  //syn->checksum = crc32(&synack, sizeof(synack));                             //add checksum
  bytes_sent = sendto(socket->sd, &syn, sizeof((syn)), MSG_CONFIRM, address, address_len); //send segment
  
  if(bytes_sent != sizeof(syn))
  {
    perror("none or not all bytes of syn were sent\n"); 
    socket->state = INVALID;
    return socket->sd;
  } 
  socket->seq_number += 1;
  socket->packets_send += 1;
  socket->bytes_send += bytes_sent;

  //wait to receive the SYNACK from the specific address
  do{
    ret = recvfrom(socket->sd, tmp_buf, MICROTCP_RECVBUF_LEN, MSG_WAITALL, &src_addr, &src_addr_length);
  }while(!is_equal_addresses(*address, src_addr));
  
  synack = get_hbo_header((microtcp_header_t *)&tmp_buf);

  // received segment
  if(ret<=0){
    socket->state = ;
    return socket->sd;
  }

  // check if checksum in received header is valid
  if(!is_checksum_valid(socket->recvbuf, ret)){
    socket->state = INVALID;
    return socket->sd;
  }

  // check that SYN and ACK bits are set to 1
  // check if ACK_received = SYN_sent + 1
  if( !is_header_control_valid(&synack, 1, 0, 1, 0)
   || synack.ack_number != socket->seq_number)
  {
    socket->state = INVALID;
    return socket->sd;
  }
  //received valid SYNACK
  socket->address = *address;
  socket->address_len = address_len;
  socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN * sizeof(uint8_t));
  socket->state = ESTABLISHED;  
  socket->ack_number = synack.seq_number + 1;

  //make header of last ack
  ack = make_header(socket->seq_number, socket->ack_number, MICROTCP_WIN_SIZE, 0, 1, 0, 0, 0);
  //ack->checksum = crc32(&synack, sizeof(synack)); //add checksum

  //send last ack
  bytes_sent = sendto(socket->sd, &ack, sizeof(ack), MSG_CONFIRM, address, address_len);
  if(bytes_sent != sizeof(ack)){
    socket->state = INVALID;
    perror("none or not all ack bytes were sent");
    return socket->sd;
  } 
  socket->seq_number += 1; 

  return socket->sd;
}


/* microtcp.h: microtcp_access returns 0 on success */

int
microtcp_accept (microtcp_sock_t *socket, struct sockaddr *address,
                 socklen_t address_len)
{
  socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN * sizeof(uint8_t));
  socket->buf_fill_level = 0;
  socket->init_win_size = MICROTCP_WIN_SIZE;
  socket->curr_win_size = MICROTCP_WIN_SIZE;
  
  microtcp_header_t syn, synack, ack;
  struct sockaddr src_addr;
  socklen_t src_addr_length;
  ssize_t bytes_sent, ret; 

  //receive SYN segment from any address
  do
  {
    ret = recvfrom(socket->sd, socket->recvbuf, MICROTCP_RECVBUF_LEN, MSG_WAITALL, &src_addr, &src_addr_length);
    if (ret > 0)
      syn = get_hbo_header((microtcp_header_t *)socket->recvbuf);
  } while (!is_header_control_valid(&syn, 0, 0, 1, 0));
  
  //received SYN segment

  // checksum validation
  if(!is_checksum_valid(socket->recvbuf, ret)){
    perror("checksum is invalid");
    socket->state = INVALID;
    return socket->sd;
  }

  //received valid SYN segment
  srand(time(NULL));
  socket->seq_number = rand(); //create random sequence number
  socket->ack_number = syn.ack_number+1;
  socket->init_win_size = syn.window;
  socket->curr_win_size = syn.window;
  socket->address = src_addr;
  socket->address_len = src_addr_length;

  //create header of SYNACK
  synack = make_header(socket->seq_number, socket->ack_number, MICROTCP_WIN_SIZE, 0, 1, 0, 1, 0);
  //synack.checksum = htonl(crc32(&synack, sizeof(synack)));

  //send SYNACK
  bytes_sent = sendto(socket->sd, &synack, sizeof(synack), MSG_CONFIRM, &socket->address, socket->address_len);
  //check that SYNACK was successfully sent
  if (bytes_sent != sizeof(synack))
  {
    socket->state = INVALID;
    perror("none or not all bytes of synack were sent\n");
    return socket->sd;
  }
  socket->seq_number += 1;
  socket->bytes_send += bytes_sent;
  socket->packets_send += 1;

  do
  {
    ret = recvfrom(socket->sd, socket->recvbuf, MICROTCP_RECVBUF_LEN, MSG_WAITALL, &src_addr, &src_addr_length);
  } while (!is_equal_addresses(socket->address, src_addr));
  
  //recvfrom failed
  if (ret <= 0)
  {
    socket->state = INVALID;
    perror("none or not all bytes of ACK were received\n");
    return socket->sd;
  }

  ack = get_hbo_header((microtcp_header_t *)socket->recvbuf);

  if(!is_checksum_valid(socket->recvbuf, recv)){
    perror("checksum is invalid");
    socket->state = INVALID;
    return socket->sd;
  }

  //check ACK bit
  if(!is_header_control_valid(&ack, 1, 0, 0, 0))
  {
    socket->state = INVALID;
    perror("failed to accept connection\n");
    return socket->sd;
  }
  socket->state = ESTABLISHED;
  socket->ack_number = ack.seq_number+1;
  
  return socket->sd;
}

int
microtcp_shutdown (microtcp_sock_t *socket, int how)
{
  microtcp_header_t finack, ack;
  ssize_t ret;
  uint32_t checksum_received, checksum_calculated;

  if(how == SHUT_RDWR){

    //SEND FINACK, RECEIVE ACK
    /* create FIN ACK segment */
    finack = make_header(socket->seq_number, socket->ack_number, MICROTCP_WIN_SIZE, 0, 1, 0, 0, 1);
    //finack.checksum = htonl(crc32(&finack, sizeof(finack)));
    
    /* send FIN ACK to client */
    ret = sendto(socket->sd, &finack, sizeof(finack), 0, &socket->address, socket->address_len);
    /* server creates FIN ACK segment */

    /* if sendto returned error value or not all header bytes were sent return invalid socket */
    if(ret != sizeof(finack))
    {
      socket->state = INVALID;
      return socket->sd;
    }

    /* wait to receive ACK from client */
    ret = recvfrom(socket->sd, socket->recvbuf, MICROTCP_RECVBUF_LEN, MSG_WAITALL, &socket->address, &socket->address_len);
    
    /* if recvfrom returned error value or not all header bytes were received return invalid socket */
    if(ret <= 0)
    {
      socket->state = INVALID;
      return socket->sd;
    }

    /* ACK header received in recv buffer */
    //  checksum_received = ntohl(server_header->checksum);
      
    /* check if checksum is valid */
    if(!is_checksum_valid(socket->recvbuf, ret))
    {
        socket->state = INVALID;
        return socket->sd;
      }
    ack = get_hbo_header((microtcp_header_t *)socket->recvbuf);

    /* check that seq number and ack number are valid */
    if(ack.seq_number != socket->ack_number || ack.ack_number != socket->seq_number
    || !is_header_control_valid(&ack, 1, 0, 0, 0))
    {
      perror("error");
      socket->state = INVALID;
      return socket->sd;
    }
    socket->seq_number += 1;

    //RECEIVE FINACK, SEND ACK
    if(socket->state != CLOSING_BY_PEER){

      socket->state = CLOSING_BY_HOST;
      
      /* wait to receive FIN ACK */
      ret = recvfrom(socket->sd, socket->recvbuf, MICROTCP_RECVBUF_LEN, MSG_WAITALL, &socket->address, &socket->address_len);

      if(ret<=0)
      {
        socket->state = INVALID;
        return socket->sd;       
      }

      /* check if checksum is valid */
      if(!is_checksum_valid(socket->recvbuf, ret)){
        socket->state = INVALID;
        return socket->sd;
      }

      finack = get_hbo_header((microtcp_header_t*)socket->recvbuf);

      /* check that FIN and ACK bits are set to 1 */
      if(finack.ack_number != socket->seq_number ||
      !is_header_control_valid(&finack, 1,0,0,1)){
        perror("error");
        socket->state = INVALID;
        return socket->sd;
      }

      socket->ack_number = finack.seq_number + 1;

      /* client creates ACK segment to send to server */
      ack = make_header(socket->seq_number, socket->ack_number, MICROTCP_WIN_SIZE, 0, 1, 0, 0, 0);
      //ack.checksum =  crc32(&ack, sizeof(ack));

      /* send ACK to server */
      ret = sendto(socket->sd, &ack, sizeof(ack), 0, &socket->address, socket->address_len);
      
      /* if sendto returned error value or not all header bytes were sent return invalid socket */
      if(ret < 0){
          socket->state = INVALID;
          return socket->sd;
      }
    }
    
    socket->state = CLOSED;
    free(socket->recvbuf);
    return socket->sd;
  }
  return socket->sd;
}

ssize_t
microtcp_send (microtcp_sock_t *socket, const void *buffer, size_t length,
               int flags)
{
  /* Your code here */
}

ssize_t
microtcp_recv (microtcp_sock_t *socket, void *buffer, size_t length, int flags)
{
  /* Your code here */
}
