test
libopenssl0_9_8-0.9.8j-0.74.1.11763.0.PTF.991722.x86_64.rpm

https://www.suse.com/zh-cn/support/kb/doc/?id=000018760
https://unix.stackexchange.com/questions/345483/how-to-set-the-guest-hardware-time-for-qemu-from-libvirt


linux-4.12.14-122.17
net/ipv6/raw.c:
1017 static int do_rawv6_setsockopt(struct sock *sk, int level, int optname,
1018                             char __user *optval, unsigned int optlen)
1019 {
1020         struct raw6_sock *rp = raw6_sk(sk);
1021         int val;
1022 
1023         if (get_user(val, (int __user *)optval))
1024                 return -EFAULT;
1025 
1026         switch (optname) {
1027         case IPV6_HDRINCL:
1028                 if (sk->sk_type != SOCK_RAW)
1029                         return -EINVAL;
1030                 inet_sk(sk)->hdrincl = !!val;
1031                 return 0;

    
    include/uapi/linux/in6.h:200:#define IPV6_HDRINCL		36
        
        
        https://linux.die.net/man/3/rmdir

        
 man setsockopt
        
RETURN VALUE
       On success, zero is returned for the standard options.  On error, -1 is returned, and errno is set appropriately.

       Netfilter  allows  the programmer to define custom socket options with associated handlers; for such options, the return value on suc-
       cess is the value returned by the handler.
           
 static int do_rawv6_setsockopt(struct sock *sk, int level, int optname,
                            char __user *optval, unsigned int optlen)
{
        struct raw6_sock *rp = raw6_sk(sk);
        int val;

        if (get_user(val, (int __user *)optval))
                return -EFAULT;

        switch (optname) {
        case IPV6_HDRINCL:
                if (sk->sk_type != SOCK_RAW)
                        return -EINVAL;
                inet_sk(sk)->hdrincl = !!val;
                return 0;
        case IPV6_CHECKSUM:
                if (inet_sk(sk)->inet_num == IPPROTO_ICMPV6 &&
                    level == IPPROTO_IPV6) {
                        /*
                         * RFC3542 tells that IPV6_CHECKSUM socket
                         * option in the IPPROTO_IPV6 level is not
                         * allowed on ICMPv6 sockets.
                         * If you want to set it, use IPPROTO_RAW
                         * level IPV6_CHECKSUM socket option
                         * (Linux extension).
                         */
                        return -EINVAL;
                }

                /* You may get strange result with a positive odd offset;
                if (val > 0 && (val&1))
                        return -EINVAL;
                if (val < 0) {
                        rp->checksum = 0;
                } else {
                        rp->checksum = 1;
                        rp->offset = val;
                }

                return 0;

        default:
                return -ENOPROTOOPT;
        }
}

errno 返回92
include/uapi/asm-generic/errno.h #define	ENOPROTOOPT	92	/* Protocol not available */

           

        https://stackoverflow.com/questions/18055195/getting-the-ipv6-header-as-part-of-the-packet
                
                https://linux.die.net/man/3/rmdir

                
                * Thu Jun 20 2013 neilb@suse.de
- patches.fixes/SUNRPC-Prevent-an-rpc_task-wakeup-race.patch:
  SUNRPC: Prevent an rpc_task wakeup race (bnc#825591).
- commit ba99fcb

      
      https://bugzilla.suse.com/show_bug.cgi?id=825591
      
  We have just released a kernel for SUSE Linux Enterprise 11 SP3 that mentions/fixes this bug. The released kernel versions is 3.0.82-0.7.9.    
