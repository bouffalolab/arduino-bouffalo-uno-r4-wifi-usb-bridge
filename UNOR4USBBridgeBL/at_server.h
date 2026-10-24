#pragma once

#include "Arduino.h"

/*
 * WiFiS3 AT 线协议的服务端：RA4M1 的 WiFiS3 库经 Serial2 发命令，这里调用平台 WiFi 库执行。
 * 已实现联网、扫描、TCP 客户端与服务端、UDP（不含组播）、TLS 客户端用到的命令与 RESTARTBOOTLOADER，其余命令回 "+<命令>:ERROR"。
 * 协议、命令子集和对上游的偏离见工作区 docs/reference/wifis3-at-protocol.md。Wi-Fi 与 socket 调用都在调用 at_serve() 的任务中进行。
 */

/* 处理 io 上已收到的字节，逐行执行命令；AT+CLIENTSEND 的载荷在函数内读完。只在一个任务中反复调用 */
void at_serve(Stream &io);
