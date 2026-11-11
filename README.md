# PU口袋校园活动自动报名工具-全自动版

大学时光如此宝贵, 不应该用在*刷软件*等浪费时间的地方! **学习**, **旅游**, **打游戏**, **社交**, 均需要大量时间! 故开发此工具, 让代码帮你*刷软件*! 解放你于相同的无趣操作!

**记得电脑对时**!! 活动等待报名时间是按本地电脑时间计算的! 程序将仅读取不需审核(`"isAudit":[0]`)的活动!  
程序将在指定时间段的每指定间隔请求活动列表, 并筛选出你可以报名(仅针对学院,未筛选年级), 学分不够(用户指定排除), 活动时间在课表课程范围外(需导入课表)的活动, **并在能报名时自动报名**! 并通过ntfy推送成果到你的设备, 活动开始前十五分钟也会给你发开始提醒! 记得查看自动报名的活动是否参加, 不参加一定要及时取消!  

活动时间冲突将仍然继续报名, 用户可以自行筛选想要的, 并手动取消不要的活动!(先报了再说!!)

**福利**: 现在还赠送一个`myqrcode.html`, 本地浏览器就可以**生成签到二维码**! 只需提供登录用户名+uid即可; 更简单的方式是直接上传你的二维码截图, 过期也可以!  

- 补1: 如果你周末不想参加活动, 可以自行在课表加上一个完整周末的课程! 课表示例见`mycourse.example.json`, 可以让ai按你的课表帮你生成相同格式的课表!  
- 补2: 还打算开发一个自动报名仅签到的活动, *但是毕竟部分组织者**可能会组织其他形式的签退***, 按签退名单, 轻的将你移出活动(不扣分), 重的直接取消你的签到(按没参加记,扣分), 故暂未开发此功能.  
- 补3: 为什么报名错误, 或要审核, 或报名定时慢了好久---活动发布者修改了活动!! 更新:每次刷新列表都会核对缓存中的活动名称,学分,时间; 发现变化时会重新获取活动详情. 但是其他不会获取(例如院系`college`等), 仍然小概率出错.  

## 配置文件

程序读取当前目录的 `config.json`. 示例(实际运行不可包含注释,这里注释只是方便用户理解):

```json5
{
  "username": "", //登录学号
  "password": "", //密码
  "sid": 243205312479232, //示例, 这是njucm的id, 请换为自己id
  "college": "人工智能与信息技术学院",//你的院系
  "excluded_credits": "文体社科||思想成长",//不参加的学分
  "advance_ms": 150,//目前不用改, 可以用
  "service_refresh_minutes": 28,//服务模式时每28分钟获取一次活动表
  "service_rest_time": "23:00-8:00",//服务模式时这个时间段不请求活动
  "service": false,//是否启动服务模式
  "ntfy_server": "https://ntfy.sh/",//ntfy服务器地址
  "ntfy_topic": "MyPUSignup",//ntfy topic
  "ntfy_username": "",
  "ntfy_password": ""
}
```

你认为学分够了就可以`excluded_credits`填写完整学时类别名称, 以 `||` 分隔; `college` 必须填写本人学院(以pu平台展示为准)的完整名称.

`service_refresh_minutes` 配置服务模式刷新活动列表间隔, 默认28分钟. `service_rest_time` 配置不请求活动列表的休息时段, 例如 `23:00-8:00`(当程序在该时段开启, 仍然会读取一次活动表);

服务模式配置 `service` 为 `true` 或启动时使用 `--service`. 服务模式无限运行, **此时无法手动单独报名活动**, 仅会自动报名任何符合条件的活动.

ntfy 配置使用 `ntfy_server`、`ntfy_topic`、`ntfy_username`、`ntfy_password`. 若不写用户名密码则尝试匿名通知, 若填写了则使用官方默认鉴权方式进行鉴权(:分隔用户名密码+base64的头). 若不写服务器和topic则不通知.

开发时备注:
接口返回 `code: 9405` 且消息为"参与人数已达上限"时, 会立即终止报名, 不再继续重试.
服务端返回 `code: 500` 且消息为"请勿频繁点击"时, 也会立即终止当前活动的报名 (这个按理可以停1.5s再报, 但这里不完全实现, 避免封号);

## 使用教程

把`config.json`配置和可选的`mycourse.json`课表放在目录之后:

- `pu-signup-tiny --signup <activity_id>`: 执行指定活动的自动报名
- `pu-signup-tiny --list`: 列出可能报名的活动; 课表加载失败时不使用课表.
- `pu-signup-tiny --service`: 无限运行, 按配置刷新配置和可报名表
- `pu-signup-tiny --help`: 显示帮助
- `--no-course`: 指定不使用课表筛选
- `--ignore-version`: 不检查版本(没必要,我们检查版本是按pu网页版返回的版本号判定版本,不代表接口更新,并且只会多输出一行,不影响执行)

## 编译

源码使用 CMake 构建, 支持 Windows 和 Linux, 要求 C++17 和 CMake 3.20 或更高版本.
网络请求统一封装在 `src/http.*`: Linux 使用系统 `libcurl`, Windows 使用系统 WinHTTP;
加密使用系统库 OpenSSL(Linux) 或 BCrypt/Crypt32 (Windows) .

### Windows

安装 MinGW-w64、CMake 和 Ninja, 并确保 `cmake`、`ninja` 和 MinGW 的编译器已经加入 PATH.
使用 Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux

安装库, 以 Ubuntu/Debian 为例:

```bash
sudo apt update
sudo apt install build-essential cmake libcurl4-openssl-dev libssl-dev
```

然后编译:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

生成的程序位于 Linux 的 `build/pu-signup-tiny`, Windows 的
`build/pu-signup-tiny.exe`.
构建完成后, 将 `config.json` 和可选的 `mycourse.json` 放到程序运行目录. 课表格式可参考仓库中的
`mycourse.example.json`.

### 打包

还可以使用 CPack 生成 ZIP 发布包(没必要):

```bash
cmake --build build --target package
```

## 其他一些

### Sid获取

get请求访问`https://apis.pocketuni.net/uc/school/list`, json中获取id.

### 活动id获取

点开喜欢的活动例如`https://class.pocketuni.net/#/activity/detail?id=375177924706305`, 后面的就是活动id!

### 写在后面

本项目代码主要由AI书写+自己优化! key通过断点获取(代码引用了某npm库"禁用"了开发者工具,处理方法很简单,override把开关关了即可!). 扒api好累! 因为返回json太乱了好多项都不知道是什么! 代码也优化了好多代, 出现了一堆完全没想到的bug, 这个是可以运行的最终版本!  
项目使用c++: 节约内存, 节约空间, 方便非计算机专业学生使用! 并且更好在服务器上编译,长久运行.  
MIT协议, 快去发给同学, 或增加自己喜欢的功能吧!

为什么我们不开发GUI: 有必要吗?? 虽然没学过计算机, 但是配置文件极其简单! 你只要输入你的账号密码学校id即可! 如果真的不会操作...你把这个项目(当然也可以包括你的课表)发给ai, 让他帮你配置! (当然还是建议自己打包, release里面的运行库不一定全)  
