#pragma once

#include <string>

class SystemChecker {
public:
    SystemChecker();
    ~SystemChecker();
    
    // 检查系统是否满足启动条件
    bool checkStartupConditions();
    
    // 检查是否在multi-user.target
    bool isMultiUserTarget();
    
    // 检查graphical.target是否未启动或不活跃
    bool isGraphicalTargetInactive();
    
    // 检查是否有DSI显示连接
    bool hasDSIDisplay();
    
private:
    // 执行systemctl命令并获取结果
    std::string executeCommand(const std::string& command);
    
    // 检查systemd单元状态
    bool isUnitActive(const std::string& unit_name);
    
    // 检查当前默认目标
    std::string getCurrentTarget();
}; 