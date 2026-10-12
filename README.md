# PU自动抢指定活动-c++版

记得电脑对时!! 活动等待报名时间是按本地电脑时间计算的!

## 教程

main.exe \<username> \<password> \<sid> \<activity_id>
例如: main.exe 084026100 password(若含特殊字符请半角直引号包裹) 243205312479232(njucm的sid) 375177924706305(要报名活动id)

## 编译

``g++ -std=c++17 -O2 main.cpp -o main.exe -lwinhttp -static-libgcc -static-libstdc++``

## Sid获取

访问`https://apis.pocketuni.net/uc/school/list`, json中获取id.

## 活动id获取

点开喜欢的活动例如`https://class.pocketuni.net/#/activity/detail?id=375177924706305`, 后面的就是活动id!

## 其他

本项目代码由AI书写+自己优化! key通过断点获取(代码引用了某npm库"禁用"了开发者工具,处理方法自行解决). (2026年10月12日测试可用! 而且该key在其他项目里也有出现!)
项目使用c++: 节约内存, 节约空间, 方便非计算机专业学生使用!  
部分库未引用直接让AI写函数: 普通大学计算机课程一般不讲g++,msvc之类的详细用法,使用者可能不会操作!
开源协议为MIT! 欢迎发给同学!  
