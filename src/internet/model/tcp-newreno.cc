/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2010 Adrian Sai-wah Tam
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Adrian Sai-wah Tam <adrian.sw.tam@gmail.com>
 */

#define NS_LOG_APPEND_CONTEXT \
  if (m_node) { std::clog << Simulator::Now ().GetSeconds () << " [node " << m_node->GetId () << "] "; }

#include "tcp-newreno.h"
#include "ns3/log.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/simulator.h"
#include "ns3/abort.h"
#include "ns3/node.h"
#include "ns3/ipv4-end-point.h"
#include "ns3/ipv4-l3-protocol.h"

NS_LOG_COMPONENT_DEFINE ("TcpNewReno");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (TcpNewReno);

TypeId
TcpNewReno::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpNewReno")
    .SetParent<TcpSocketBase> ()
    .AddConstructor<TcpNewReno> ()
    .AddAttribute ("ReTxThreshold", "Threshold for fast retransmit",
                    UintegerValue (3),
                    MakeUintegerAccessor (&TcpNewReno::m_retxThresh),
                    MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("LimitedTransmit", "Enable limited transmit",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpNewReno::m_limitedTx),
                   MakeBooleanChecker ())
    .AddAttribute ("dctcp", "Enable DCTCP",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpNewReno::m_dctcp),
                   MakeBooleanChecker ())
    .AddTraceSource ("CongestionWindow",
                     "The TCP connection's congestion window",
                     MakeTraceSourceAccessor (&TcpNewReno::m_cWnd))
    .AddTraceSource ("SlowStartThreshold",
                     "TCP slow start threshold (bytes)",
                     MakeTraceSourceAccessor (&TcpNewReno::m_ssThresh))
 ;
  return tid;
}

TcpNewReno::TcpNewReno (void)
  : m_retxThresh (3), // mute valgrind, actual value set by the attribute system
    m_inFastRec (false),
    m_limitedTx (false) // mute valgrind, actual value set by the attribute system
{
  NS_LOG_FUNCTION (this);
  highest_ack_recvd = 0;
  beta = 1.0/32.0;
  bytes_with_ecn = total_bytes_acked = 0.0;
  dctcp_alpha = 0.0;
  dctcp_reacted = false;
  xfabric_reacted = false;
  d0 = 0.00003; //30us - TBD
  dt = d0 * 1.0;

}

TcpNewReno::TcpNewReno (const TcpNewReno& sock)
  : TcpSocketBase (sock),
    m_cWnd (sock.m_cWnd),
    m_ssThresh (sock.m_ssThresh),
    m_initialCWnd (sock.m_initialCWnd),
    m_initialSsThresh (sock.m_initialSsThresh),
    m_retxThresh (sock.m_retxThresh),
    m_inFastRec (false),
    m_limitedTx (sock.m_limitedTx)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_LOGIC ("Invoked the copy constructor");
}

TcpNewReno::~TcpNewReno (void)
{
}

/* We initialize m_cWnd from this function, after attributes initialized */
int
TcpNewReno::Listen (void)
{
  NS_LOG_FUNCTION (this);
  InitializeCwnd ();
  return TcpSocketBase::Listen ();
}

/* We initialize m_cWnd from this function, after attributes initialized */
int
TcpNewReno::Connect (const Address & address)
{
  NS_LOG_FUNCTION (this << address);
  InitializeCwnd ();
  return TcpSocketBase::Connect (address);
}

/* Limit the size of in-flight data by cwnd and receiver's rxwin */
uint32_t
TcpNewReno::Window (void)
{
  NS_LOG_FUNCTION (this);
  return std::min (m_rWnd.Get (), m_cWnd.Get ());
}

Ptr<TcpSocketBase>
TcpNewReno::Fork (void)
{
  return CopyObject<TcpNewReno> (this);
}

/* New ACK (up to seqnum seq) received. Increase cwnd and call TcpSocketBase::NewAck() */
void
TcpNewReno::NewAck (const SequenceNumber32& seq)
{
  NS_LOG_FUNCTION (this << seq);
  NS_LOG_LOGIC ("TcpNewReno received ACK for seq " << seq <<
                " cwnd " << m_cWnd <<
                " ssthresh " << m_ssThresh);

  // Check for exit condition of fast recovery
  if (m_inFastRec && seq < m_recover)
    { // Partial ACK, partial window deflation (RFC2582 sec.3 bullet #5 paragraph 3)
      m_cWnd += m_segmentSize - (seq - m_txBuffer.HeadSequence ());
      NS_LOG_INFO ("Partial ACK in fast recovery: cwnd set to " << m_cWnd);
      m_txBuffer.DiscardUpTo(seq);  //Bug 1850:  retransmit before newack
      DoRetransmit (); // Assume the next seq is lost. Retransmit lost packet
      TcpSocketBase::NewAck (seq); // update m_nextTxSequence and send new data if allowed by window
      return;
    }
  else if (m_inFastRec && seq >= m_recover)
    { // Full ACK (RFC2582 sec.3 bullet #5 paragraph 2, option 1)
      m_cWnd = std::min (m_ssThresh.Get (), BytesInFlight () + m_segmentSize);
      m_inFastRec = false;
      NS_LOG_INFO ("Received full ACK. Leaving fast recovery with cwnd set to " << m_cWnd);
    }

  // Increase of cwnd based on current phase (slow start or congestion avoidance)
  if (m_cWnd < m_ssThresh)
    { // Slow start mode, add one segSize to cWnd. Default m_ssThresh is 65535. (RFC2001, sec.1)
      m_cWnd += m_segmentSize;
      NS_LOG_INFO ("In SlowStart, updated to cwnd " << m_cWnd << " ssthresh " << m_ssThresh);
      std::cout<<"In SlowStart, updated to cwnd " << m_cWnd << " ssthresh " << m_ssThresh<<std::endl;
    }
  else
    { 
      if(!xfabric_reacted) {
        // Congestion avoidance mode, increase by (segSize*segSize)/cwnd. (RFC2581, sec.3.1)
        // To increase cwnd for one segSize per RTT, it should be (ackBytes*segSize)/cwnd
        double adder = static_cast<double> (m_segmentSize * m_segmentSize) / m_cWnd.Get ();
        adder = std::max (1.0, adder);
        m_cWnd += static_cast<uint32_t> (adder);
        xfabric_reacted = false;
        std::cout<<"In CongAvoid, updated to cwnd " << m_cWnd << " ssthresh " << m_ssThresh<<" node "<<m_node->GetId()<<" at time "<<Simulator::Now().GetSeconds()<<std::endl;
      }
    }
  TcpSocketBase::NewAck (seq);
}

uint32_t 
TcpNewReno::getFlowId(std::string flowkey)
{
  uint32_t fid = 0;
  Ptr<Ipv4L3Protocol> ipv4 = StaticCast<Ipv4L3Protocol > (m_node->GetObject<Ipv4> ());
  if((ipv4->flowids).find(flowkey) != (ipv4->flowids).end()) {
    fid = ipv4->flowids[flowkey];
  }
   
//  std::map<std::string, uint32_t>::iterator it;
//  for (it=ipv4->flowids.begin(); it!=ipv4->flowids.end(); ++it) {
//    std::cout<<it->first<<" "<<it->second<<std::endl;
//  }
 
//  std::cout<<" flowkey "<<flowkey<<" fid "<<fid<<std::endl; 
  return fid;
}

void
TcpNewReno::processRate(const TcpHeader &tcpHeader)
{
  double target_rate = tcpHeader.GetRate();
  std::stringstream ss;
  ss<<m_endPoint->GetLocalAddress()<<":"<<m_endPoint->GetPeerAddress()<<":"<<m_endPoint->GetPeerPort();
  std::string flowkey = ss.str();
 
  double res = target_rate * (1000000.0/8.0) * 0.000035; //TBD - dt from commandline

  m_cWnd = ((uint32_t) (res/m_segmentSize) + 1) * m_segmentSize; //TBD

//  std::cout<<"m_cWnd "<<m_cWnd<<" res "<<res<<std::endl; 

  if(m_cWnd < 1* m_segmentSize) 
  {
    m_cWnd = 1 * m_segmentSize;
  }
  m_ssThresh = m_cWnd;
  if(flowkey == "10.1.0.1:10.1.2.2:2") {
    std::cout<<" flow "<<  flowkey << " target rate "<<target_rate<<" nodeid "<<m_node->GetId()<<" cWnd "<<m_cWnd<<std::endl;
  }
  xfabric_reacted = true;
}
   
 
void
TcpNewReno::ProcessECN(const TcpHeader &tcpHeader)
{

  NS_LOG_FUNCTION (this << tcpHeader);
  SequenceNumber32 ack_num = tcpHeader.GetAckNumber();


  /* On every ACK, we want to estimate new window for strawman CC */
  std::stringstream ss;
  ss<<m_endPoint->GetLocalAddress()<<":"<<m_endPoint->GetPeerAddress()<<":"<<m_endPoint->GetPeerPort();
  std::string flowkey = ss.str();
  //std::cout<<Simulator::Now().GetSeconds()<<"node "<<m_node->GetId()<<" flow "<<flowkey<<" rtt "<<lastRtt_usable.GetSeconds()<<std::endl;

  /* update the last ack recvd variable */
  int32_t num_bytes_acked = ack_num.GetValue() - highest_ack_recvd.GetValue();
  NS_LOG_INFO("DCTCP_DEBUG "<<highest_ack_recvd.GetValue()<<" ack_num "<<ack_num.GetValue());
  NS_LOG_INFO(Simulator::Now().GetSeconds()<<" DCTCP_DEBUG num_bytes_acked "<<num_bytes_acked<<" highest_ack_recd "<<highest_ack_recvd<<" total_bytes_acked "<<total_bytes_acked);
  highest_ack_recvd = ack_num;
  total_bytes_acked += num_bytes_acked;
  double new_alpha = 0.0;

  if(tcpHeader.GetECN() == 1) {
    bytes_with_ecn += num_bytes_acked;
  }
  //if(total_bytes_acked > (int)m_cWnd) {
  if(ack_num >= last_outstanding_num) {
     if(total_bytes_acked > 0.0) {
       new_alpha = double((bytes_with_ecn * 1.0) / (total_bytes_acked * 1.0));
       dctcp_alpha = (1.0 - beta)*dctcp_alpha + beta* new_alpha;
       total_bytes_acked = 0;
       bytes_with_ecn = 0; 
       std::cout<<Simulator::Now().GetSeconds()<<" node "<<m_node->GetId()<<" DCTCP_DEBUG new_alpha "<<new_alpha<<" DCTCP_ALPHA "<<dctcp_alpha<<" "<<flowkey<<" fid "<<getFlowId(flowkey)<<std::endl;
     }
     last_outstanding_num = m_highTxMark;
   }
  
  
  if(tcpHeader.GetECN() == 1) {
    /* If the mark is on a packet which is acking a packet lesser than highest tx number,
     * don't react */
 //   NS_LOG_INFO("ProcessECN .. ACK recvd for seq "<<ack_num<<" ecn_highest set to "<<ecn_highest<< "current highest "<<m_highTxMark); 

    if(m_dctcp) {
      //NS_LOG_UNCOND("bytes_with_ecn "<<bytes_with_ecn<<" totalbytes "<<total_bytes_acked);
      if(ack_num >= ecn_highest) {

          //NS_LOG_UNCOND("m_smoother_dctcp is false");
          m_cWnd = (m_cWnd) * (1.0 - dctcp_alpha/2.0);

          if(m_cWnd < 1* m_segmentSize) 
          {
            m_cWnd = 1 * m_segmentSize;
          }
          m_ssThresh = m_cWnd;
          dctcp_reacted = true;

          /* note that we won't react for another RTT */
          ecn_highest = m_highTxMark;
          //bytes_with_ecn = 0.0;
          //total_bytes_acked = 0.0;
        
      } else {
  //      NS_LOG_INFO("Notreacting "<<Simulator::Now().GetSeconds());
        // no reaction 
      }
    } else {
    
      /*
      if(ack_num < ecn_highest) {
        NS_LOG_INFO("ecn mark recvd. but, it is not yet time to react");
        return;
      } 
      */
  
      /* On receipt of ECN marked ACK, cut the congestion window by half */
      //  m_ssThresh = std::max (2 * m_segmentSize, BytesInFlight () / 2);
      //  m_cWnd = m_ssThresh + 3 * m_segmentSize;

      double adder = static_cast<double> (m_segmentSize) / 2;
      adder = std::max (1.0, adder);
     // m_cWnd -= static_cast<uint32_t> (adder);

      //NS_LOG_UNCOND("m_cWnd before : "<<m_cWnd); 
      m_cWnd = (m_cWnd *19)/20;
      //NS_LOG_UNCOND("m_cWnd after : "<<m_cWnd); 
      if(m_cWnd < 1* m_segmentSize) 
      {
        m_cWnd = 1 * m_segmentSize;
      }
      m_ssThresh = m_cWnd;
      ecn_highest = m_highTxMark;
      //  m_inFastRec = true;
      NS_LOG_INFO ("ProcessECN. Enter fast recovery mode. Reset cwnd to " << m_cWnd <<
               ", ssthresh to " << m_ssThresh << " at fast recovery seqnum " << ecn_highest);
    }
  } 
    

}
  
  

/* Cut cwnd and enter fast recovery mode upon triple dupack */
void
TcpNewReno::DupAck (const TcpHeader& t, uint32_t count)
{
  NS_LOG_FUNCTION (this << count);
  if (count == m_retxThresh && !m_inFastRec)
    { // triple duplicate ack triggers fast retransmit (RFC2582 sec.3 bullet #1)
        m_ssThresh = std::max (2 * m_segmentSize, BytesInFlight () / 2);
        m_cWnd = m_ssThresh + 3 * m_segmentSize;
        m_recover = m_highTxMark;
        m_inFastRec = true;
        NS_LOG_INFO ("Triple dupack. Enter fast recovery mode. Reset cwnd to " << m_cWnd <<
                   ", ssthresh to " << m_ssThresh << " at fast recovery seqnum " << m_recover);
      DoRetransmit ();
    } 
    else if (m_inFastRec) 
    { // Increase cwnd for every additional dupack (RFC2582, sec.3 bullet #3)
      m_cWnd += m_segmentSize;
      NS_LOG_INFO ("Dupack in fast recovery mode. Increase cwnd to " << m_cWnd);
      SendPendingData (m_connected);
    }
  else if (!m_inFastRec && m_limitedTx && m_txBuffer.SizeFromSequence (m_nextTxSequence) > 0)
    { // RFC3042 Limited transmit: Send a new packet for each duplicated ACK before fast retransmit
      NS_LOG_INFO ("Limited transmit");
      uint32_t sz = SendDataPacket (m_nextTxSequence, m_segmentSize, true);
      m_nextTxSequence += sz;                    // Advance next tx sequence
    };
}

/* Retransmit timeout */
void
TcpNewReno::Retransmit (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_LOGIC (this << " ReTxTimeout Expired at time " << Simulator::Now ().GetSeconds ());

/*
  if(ecn_backoff_) {
     We don't need to enter 
 */
  m_inFastRec = false;

  // If erroneous timeout in closed/timed-wait state, just return
  if (m_state == CLOSED || m_state == TIME_WAIT) return;
  // If all data are received (non-closing socket and nothing to send), just return
  if (m_state <= ESTABLISHED && m_txBuffer.HeadSequence () >= m_highTxMark) return;

  // According to RFC2581 sec.3.1, upon RTO, ssthresh is set to half of flight
  // size and cwnd is set to 1*MSS, then the lost packet is retransmitted and
  // TCP back to slow start
  m_ssThresh = std::max (2 * m_segmentSize, BytesInFlight () / 2);
  m_cWnd = m_segmentSize;
  m_nextTxSequence = m_txBuffer.HeadSequence (); // Restart from highest Ack
  NS_LOG_INFO ("RTO. Reset cwnd to " << m_cWnd <<
               ", ssthresh to " << m_ssThresh << ", restart from seqnum " << m_nextTxSequence);
  m_rtt->IncreaseMultiplier ();             // Double the next RTO
  DoRetransmit ();                          // Retransmit the packet
}

void
TcpNewReno::SetSegSize (uint32_t size)
{
  NS_ABORT_MSG_UNLESS (m_state == CLOSED, "TcpNewReno::SetSegSize() cannot change segment size after connection started.");
  m_segmentSize = size;
}

void
TcpNewReno::SetInitialSSThresh (uint32_t threshold)
{
  NS_ABORT_MSG_UNLESS (m_state == CLOSED, "TcpNewReno::SetSSThresh() cannot change initial ssThresh after connection started.");
  m_initialSsThresh = threshold;
}

uint32_t
TcpNewReno::GetInitialSSThresh (void) const
{
  return m_initialSsThresh;
}

void
TcpNewReno::SetInitialCwnd (uint32_t cwnd)
{
  NS_ABORT_MSG_UNLESS (m_state == CLOSED, "TcpNewReno::SetInitialCwnd() cannot change initial cwnd after connection started.");
  m_initialCWnd = cwnd;
}

uint32_t
TcpNewReno::GetInitialCwnd (void) const
{
  return m_initialCWnd;
}

void 
TcpNewReno::InitializeCwnd (void)
{
  /*
   * Initialize congestion window, default to 1 MSS (RFC2001, sec.1) and must
   * not be larger than 2 MSS (RFC2581, sec.3.1). Both m_initiaCWnd and
   * m_segmentSize are set by the attribute system in ns3::TcpSocket.
   */
  m_cWnd = m_initialCWnd * m_segmentSize;
  m_ssThresh = m_initialSsThresh;
  std::cout<<"set initial cwnd to "<<m_cWnd<<std::endl;

}

} // namespace ns3
