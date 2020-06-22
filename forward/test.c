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
