#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

#define UDP_QUEUE_LEN 16
#define UDP_PORT_SLOTS NPROC

struct udp_packet {
  uint32 src_ip;       // host byte order
  uint16 src_port;     // host byte order
  int len;             // UDP payload length
  char *payload;       // points inside owner
  char *owner;         // page received from the E1000
};

struct udp_port_queue {
  struct spinlock lock;
  int used;
  uint16 port;         // host byte order
  int count;
  int head;
  int tail;
  struct udp_packet packets[UDP_QUEUE_LEN];
};

static struct spinlock udp_table_lock;
static struct udp_port_queue udp_ports[UDP_PORT_SLOTS];

// There is no unbind in the required API, so a returned queue pointer
// remains valid after udp_lookup() releases udp_table_lock.
static struct udp_port_queue *
udp_lookup(uint16 port)
{
  struct udp_port_queue *q = 0;

  acquire(&udp_table_lock);
  for(int i = 0; i < UDP_PORT_SLOTS; i++){
    if(udp_ports[i].used && udp_ports[i].port == port){
      q = &udp_ports[i];
      break;
    }
  }
  release(&udp_table_lock);

  return q;
}

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

void
netinit(void)
{
  initlock(&netlock, "netlock");

  initlock(&udp_table_lock, "udp-table");
  for(int i = 0; i < UDP_PORT_SLOTS; i++){
    initlock(&udp_ports[i].lock, "udp-port");
    udp_ports[i].used = 0;
    udp_ports[i].port = 0;
    udp_ports[i].count = 0;
    udp_ports[i].head = 0;
    udp_ports[i].tail = 0;
    memset(udp_ports[i].packets, 0, sizeof(udp_ports[i].packets));
  }
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //
  int raw_port;
  uint16 port;
  int free_slot = -1;

  argint(0, &raw_port);
  port = (uint16)raw_port;

  acquire(&udp_table_lock);

  // Treat a repeated bind as an idempotent success.
  for(int i = 0; i < UDP_PORT_SLOTS; i++){
    if(udp_ports[i].used && udp_ports[i].port == port){
      release(&udp_table_lock);
      return 0;
    }
    if(!udp_ports[i].used && free_slot < 0)
      free_slot = i;
  }

  if(free_slot < 0){
    release(&udp_table_lock);
    return -1;
  }

  struct udp_port_queue *q = &udp_ports[free_slot];
  q->port = port;
  q->count = 0;
  q->head = 0;
  q->tail = 0;
  memset(q->packets, 0, sizeof(q->packets));

  // Publish the slot only after all queue fields are initialized.
  q->used = 1;

  release(&udp_table_lock);
  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  //
  // Your code here.
  //
  int raw_dport;
  int maxlen;
  uint64 user_src_ip;
  uint64 user_src_port;
  uint64 user_buf;

  argint(0, &raw_dport);
  argaddr(1, &user_src_ip);
  argaddr(2, &user_src_port);
  argaddr(3, &user_buf);
  argint(4, &maxlen);

  if(maxlen < 0)
    return -1;

  uint16 dport = (uint16)raw_dport;
  struct udp_port_queue *q = udp_lookup(dport);
  if(q == 0)
    return -1;

  struct udp_packet pkt;

  acquire(&q->lock);
  while(q->count == 0){
    if(killed(myproc())){
      release(&q->lock);
      return -1;
    }

    // sleep atomically releases q->lock and reacquires it on wakeup.
    sleep(q, &q->lock);
  }

  // Remove exactly one oldest packet while holding the queue lock.
  pkt = q->packets[q->head];
  memset(&q->packets[q->head], 0, sizeof(q->packets[q->head]));
  q->head = (q->head + 1) % UDP_QUEUE_LEN;
  q->count--;
  release(&q->lock);

  int n = pkt.len;
  if(n > maxlen)
    n = maxlen;

  pagetable_t pagetable = myproc()->pagetable;

  if(copyout(pagetable, user_src_ip,
             (char *)&pkt.src_ip, sizeof(pkt.src_ip)) < 0 ||
     copyout(pagetable, user_src_port,
             (char *)&pkt.src_port, sizeof(pkt.src_port)) < 0 ||
     (n > 0 && copyout(pagetable, user_buf, pkt.payload, n) < 0)){
    kfree(pkt.owner);
    return -1;
  }

  kfree(pkt.owner);
  return n;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
  const int fixed_headers =
    sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);

  if(len < fixed_headers){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);

  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)(ip + 1);
  uint16 udp_len = ntohs(udp->ulen);

  // ulen includes the UDP header itself.
  if(udp_len < sizeof(struct udp) ||
     sizeof(struct eth) + sizeof(struct ip) + udp_len > len){
    kfree(buf);
    return;
  }

  uint16 dport = ntohs(udp->dport);
  uint16 sport = ntohs(udp->sport);
  uint32 src_ip = ntohl(ip->ip_src);
  int payload_len = udp_len - sizeof(struct udp);

  struct udp_port_queue *q = udp_lookup(dport);
  if(q == 0){
    // No process has bound this destination port.
    kfree(buf);
    return;
  }

  acquire(&q->lock);

  if(q->count == UDP_QUEUE_LEN){
    // The 16-packet limit applies independently to each port.
    release(&q->lock);
    kfree(buf);
    return;
  }

  struct udp_packet *pkt = &q->packets[q->tail];
  pkt->src_ip = src_ip;
  pkt->src_port = sport;
  pkt->len = payload_len;
  pkt->payload = (char *)(udp + 1);
  pkt->owner = buf;

  q->tail = (q->tail + 1) % UDP_QUEUE_LEN;
  q->count++;

  // Use q as both the queue object and the sleep/wakeup channel.
  wakeup(q);
  release(&q->lock);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
