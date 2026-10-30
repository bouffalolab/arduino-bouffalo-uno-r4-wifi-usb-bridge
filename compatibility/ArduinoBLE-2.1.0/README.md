# ArduinoBLE 2.1.0 公共地址绑定重连兼容补丁

这是RA4M1 Host library的补丁，不修改BL616CL Core、Controller或bridge协议。普通未加密GATT、官方BLE示例和既有Bumble公共地址/身份密钥分发路径仍可使用原库；需要与不发送initiator IdKey的BlueZ公共地址对端绑定重连时，应用本补丁并重新构建RA程序。

## 问题与修正

官方2.1.0把双方key distribution固定回复为IdKey(2)，没有与请求掩码求交；长期密钥保存回调只在收到Identity Address时调用。实机BlueZ请求initiator=13（不含IdKey）、responder=15；首次加密成功，但不会发送本端Identity Address，RA端未保存LTK。重新连接时getLTK返回0，Controller报告加密status6。该原版失败可以复现，原始记录不删除。

补丁将双方key distribution与本库支持的IdKey求交。若本次Secure Connections配对的公共地址对端不分发IdKey，在加密建立时按已知公共地址调用原storeLTK回调；对端分发IdKey时继续走原身份地址回调。应用仍负责正确保存/查询密钥，不提供默认成功值。

本补丁只处理已经实测的公共地址场景。RPA、没有IdKey的随机地址及掉电后密钥持久化不在此补丁验收范围。测试密钥仅放RAM，不记录LTK/IRK或数字比较码。

## 标准Arduino安装与构建

安装Renesas UNO core 1.6.0和ArduinoBLE 2.1.0。通过Arduino IDE或CLI配置找到用户sketchbook的library目录，将BLE_LIBRARY设置为其中ArduinoBLE目录；不要修改BL平台安装目录或在普通sketch构建时自动打补丁。

先用manifest.json中的原始SHA256核对HCI.cpp和L2CAPSignaling.cpp，再在本仓库根目录执行：

```sh
patch --dry-run -p1 -d "$BLE_LIBRARY" < compatibility/ArduinoBLE-2.1.0/public-peer-bond.patch
patch -p1 -d "$BLE_LIBRARY" < compatibility/ArduinoBLE-2.1.0/public-peer-bond.patch
arduino-cli compile -b arduino:renesas_uno:unor4wifi --build-path "$RA_BUILD" "$RA_SKETCH"
arduino-cli upload -b arduino:renesas_uno:unor4wifi -p "$RA_PORT" --input-dir "$RA_BUILD" "$RA_SKETCH"
```

应用后两个源文件SHA应与manifest.json的patched_sha256一致。补丁不能重复应用；版本或校验不符时停止，重新安装准确版本，不用模糊修改掩盖差异。Arduino Library Manager更新或重装会替换用户library，需重新核对，不承诺适用于未来版本。

回退可使用patch -R在同一library撤销，或重装官方2.1.0，再重建RA程序；回退后原BlueZ绑定重连限制仍存在。BL固件不需要因此重刷。

## 实机与来源

ARDUINO-53，Linux x86_64、RA4M1/Renesas1.6.0、BL616CL 1V0候选9d345c2d、真实AX200/BlueZ5.72和Bumble0.0.236。原版BlueZ路径失败、补丁版配对拒绝/接受、加密10轮读写/通知及关闭新配对后的重连通过；Bumble身份密钥分发、加密重连和窗口到期回归通过。重复重连的实际轮次和原始日志在对应task补充验收包中列明。

源文件与官方2.1.0 tag逐字节一致后再修改，原始及修改后SHA见manifest.json。保留原LGPL-2.1许可于LICENSE.ArduinoBLE；本补丁修改SMP协商及LTK保存时机，不改变第三方版权声明。尚未向Arduino上游提交，不宣称官方已合并此修正。
