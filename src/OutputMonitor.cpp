/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the LGPL v2.1 as described in the
 * included LICENSE.LGPL file.
 */

#include "fpp-pch.h"

#include "OutputMonitor.h"
#include "gpio.h"
#include "Timers.h"

OutputMonitor OutputMonitor::INSTANCE;

class PortPinInfo {
public:
    PortPinInfo(const std::string &n, const Json::Value &c) : name(n), config(c) {}
    ~PortPinInfo() {}

    std::string name;
    Json::Value config;

    bool isOn = false;
    bool hasTriggered = false;
    bool enabled = true;

    const PinCapabilities *enablePin = nullptr;
    bool highToEnable = true;
    const PinCapabilities *eFusePin = nullptr;
    int eFuseOKValue = 0;

    const PinCapabilities *eFuseInterruptPin = nullptr;

    int currentMonitorFile = -1;
    float currentMonitorScale = 1.0f;

    void appendTo(Json::Value &result) {
        Json::Value v;
        v["name"] = name;
        if (enablePin) {
            int pv = enablePin->getValue();
            v["enabled"] = (pv && highToEnable) || (!pv && !highToEnable);
        }
        if (isOn && eFusePin) {
            v["status"] = eFuseOKValue == eFusePin->getValue();
        } else {
            v["status"] = true;
        }
        if (currentMonitorFile) {
            char buf[12] = {0};
            float f = 0;
            for (int x = 0; x < 3; x++) {
                lseek(currentMonitorFile, 0, SEEK_SET);
                read(currentMonitorFile, buf, sizeof(buf));                
                f += atoi(buf);
            }
            f /= 3;
            f *= 1000;
            f /= currentMonitorScale;
            int c = std::round(f);
            v["ma"] = c;
        }
        if (config.isMember("row")) {
            v["row"] = config["row"];
        }
        if (config.isMember("col")) {
            v["col"] = config["col"];
        }
        result.append(v);
    }
};

class FPPEnableOutputsCommand : public Command {
public:
    FPPEnableOutputsCommand() :
        Command("Outputs On") {
    }
    virtual std::unique_ptr<Command::Result> run(const std::vector<std::string>& args) override {
        OutputMonitor::INSTANCE.EnableOutputs();
        return std::make_unique<Command::Result>("OK");
    }
};
class FPPDisableOutputsCommand : public Command {
public:
    FPPDisableOutputsCommand() :
        Command("Outputs Off") {
    }
    virtual std::unique_ptr<Command::Result> run(const std::vector<std::string>& args) override {
        OutputMonitor::INSTANCE.DisableOutputs();
        return std::make_unique<Command::Result>("OK");
    }
};

OutputMonitor::OutputMonitor() {

}
OutputMonitor::~OutputMonitor() {
    for (auto pi : portPins) {
        delete pi;
    }
    portPins.clear();
}


void OutputMonitor::Initialize(std::map<int, std::function<bool(int)>>& callbacks) {
    CommandManager::INSTANCE.addCommand(new FPPEnableOutputsCommand());
    CommandManager::INSTANCE.addCommand(new FPPDisableOutputsCommand());
    /*
    std::function<void()> f = [this]() {
        printf("\n\n");
        float total = 0;
        //for (int x = 0; x < 16; x++) {
        //    const PinCapabilities &p = PinCapabilities::getPinByName("pca9675-" + std::to_string(x));
        //    printf("    %d:  %d\n", x, p.getValue());
        //}
        for (auto &fp : portPins) {
            printf("%s: %d  (%s)", fp.first.c_str(), fp.second->enablePin->getValue(), fp.second->enablePin->name.c_str());
            if (fp.second->eFusePin) {
                printf("    eFuse: %d (%s)", fp.second->eFusePin->getValue(), fp.second->eFusePin->name.c_str());
            }
            if (fp.second->currentMonitorFile > 0) {
                char buf[12] = {0};
                lseek(fp.second->currentMonitorFile, 0, SEEK_SET);
                size_t i = read(fp.second->currentMonitorFile, buf, sizeof(buf));                
                float f = atoi(buf);
                f *= 1000;
                f /= fp.second->currentMonitorScale;
                total += f;
                printf("   %0.2f ma", f);
            }
            printf("\n");
        }
        if (portPins.begin()->second->eFuseInterruptPin) {
            printf("   ipv:  %d\n", portPins.begin()->second->eFuseInterruptPin->getValue());
        }
        if (total > 0.001) {
            printf("   Total:  %0.2fma\n", total);
        }
    };
    Timers::INSTANCE.addPeriodicTimer("OutputMonitor", 1000, f);
    */
}

void OutputMonitor::EnableOutputs() {
    if (!pullHighOutputPins.empty() || !pullLowOutputPins.empty()) {
        LogDebug(VB_CHANNELOUT, "Enabling outputs\n");
    }
    for (auto &p : pullHighOutputPins) {
        p->setValue(1);
    }
    for (auto &p : pullLowOutputPins) {
        p->setValue(0);
    }
    for (auto p : portPins) {
        if (p->enabled) {
            p->isOn = true;
            p->hasTriggered = false;
        }
    }
    for (auto p : portPins) {
        if (p->eFusePin->getValue() == p->eFuseOKValue) {
            WarningHolder::RemoveWarning("eFUSE Triggered for " + p->name);
        }
    }
    CommandManager::INSTANCE.TriggerPreset("OUTPUTS_ENABLED");
}
void OutputMonitor::DisableOutputs() {
    if (!pullHighOutputPins.empty() || !pullLowOutputPins.empty()) {
        LogDebug(VB_CHANNELOUT, "Disabling outputs\n");
    }
    for (auto p : portPins) {
        p->isOn = false;
    }
    for (auto &p : pullHighOutputPins) {
        p->setValue(0);
    }
    for (auto &p : pullLowOutputPins) {
        p->setValue(1);
    }
    CommandManager::INSTANCE.TriggerPreset("OUTPUTS_DISABLED");
}
void OutputMonitor::AddPortConfiguration(const std::string &name, const Json::Value &pinConfig, bool enabled) {
    PortPinInfo *pi = new PortPinInfo(name, pinConfig);
    bool hasInfo = false;
    if (pinConfig.isMember("enablePin")) {
        std::string ep = pinConfig.get("enablePin", "").asString();
        if (ep != "") {
            pi->enablePin = AddOutputPin(name, ep);
            pi->highToEnable = (ep[0] != '!');
            if (!enabled) {
                pi->enabled = false;
                if (pi->highToEnable) {
                    pullHighOutputPins.pop_back();
                } else {
                    pullLowOutputPins.pop_back();
                }
            }
            hasInfo = true;
        }
    }
    if (pinConfig.isMember("eFusePin")) {

        if (pinConfig.isMember("eFuseInterruptPin")) {
            std::string eFuseInterruptPin = pinConfig.get("eFuseInterruptPin", "").asString();
            bool eFuseInterruptHigh = false;
            if (eFuseInterruptPin[0] == '!') {
                eFuseInterruptPin = true;
                eFuseInterruptPin = eFuseInterruptPin.substr(1);
            }
            std::string postFix = "";
            if (eFuseInterruptPin[0] == '-') {
                postFix = "_pd";
                eFuseInterruptPin = eFuseInterruptPin.substr(1);
            } else if (eFuseInterruptPin[0] == '+') {
                postFix = "_pu";
                eFuseInterruptPin = eFuseInterruptPin.substr(1);
            } 
            pi->eFuseInterruptPin = PinCapabilities::getPinByName(eFuseInterruptPin).ptr();
            if (!pi->eFuseInterruptPin) {
                LogWarn(VB_CHANNELOUT, "Could not find pin " + eFuseInterruptPin + " to handle fuse interrupts for output " + name + "\n");
                WarningHolder::AddWarning("Could not find pin " + eFuseInterruptPin + " to handle fuse interrupts for output " + name);
            } else {
                hasInfo = true;
                if (fusePins[eFuseInterruptPin] == nullptr) {
                    pi->eFuseInterruptPin->configPin("gpio" + postFix, false);
                    GPIOManager::INSTANCE.AddGPIOCallback(pi->eFuseInterruptPin, [this, pi](int v) {
                        //printf("\n\n\nInterrupt Pin!!!   %d   %d\n\n\n", v, pi->eFuseInterruptPin->getValue());
                        for (auto a : portPins) {
                            if (a->eFuseInterruptPin == pi->eFuseInterruptPin) {
                                int v = a->eFusePin->getValue();
                                if (v != a->eFuseOKValue) {
                                    if (a->enablePin) {
                                        //make sure the port is turned off
                                        a->enablePin->setValue(a->highToEnable ? 0 : 1);
                                    }
                                    if (a->isOn && !a->hasTriggered) {                                        
                                        // Output SHOULD be on, but the fuse triggered.  That's a warning.
                                        LogWarn(VB_CHANNELOUT, "eFUSE Triggered for " + a->name + "\n");
                                        WarningHolder::AddWarning("eFUSE Triggered for " + a->name);
                                        a->hasTriggered = true;
                                    }
                                }                            
                            }
                        }
                        return true;
                    });
                }
            }
        }
        std::string eFusePin = pinConfig.get("eFusePin", "").asString();
        bool eFuseHigh = false;
        if (eFusePin[0] == '!') {
            eFuseHigh = true;
            eFusePin = eFusePin.substr(1);
        }
        std::string postFix = "";
        if (eFusePin[0] == '-') {
            postFix = "_pd";
            eFusePin = eFusePin.substr(1);
        } else if (eFusePin[0] == '+') {
            postFix = "_pu";
            eFusePin = eFusePin.substr(1);
        } 
        pi->eFusePin = PinCapabilities::getPinByName(eFusePin).ptr();
        pi->eFuseOKValue = eFuseHigh ? 1 : 0;
        if (pi->eFusePin == nullptr) {
            LogWarn(VB_CHANNELOUT, "Could not find pin " + eFusePin + " to handle fuse for output " + name + "\n");
            WarningHolder::AddWarning("Could not find pin " + eFusePin + " to handle fuse for output " + name);
        } else {
            pi->eFusePin->configPin("gpio" + postFix, false);
            if (pi->eFuseInterruptPin == nullptr) {
                GPIOManager::INSTANCE.AddGPIOCallback(pi->eFusePin, [this, pi](int v) {
                    //printf("eFuse for %s trigger: %d    %d\n",  pi->name.c_str(), v, pi->eFusePin->getValue());
                    if (v != pi->eFuseOKValue) {
                        if (pi->enablePin) {
                            //make sure the port is turned off
                            pi->enablePin->setValue(pi->highToEnable ? 0 : 1);
                        }
                        if (pi->isOn) {
                            LogWarn(VB_CHANNELOUT, "eFUSE Triggered for " + pi->name + "\n");
                            // Output SHOULD be on, but the fuse triggered.  That's a warning.
                            WarningHolder::AddWarning("eFUSE Triggered for " + pi->name);

                            std::map<std::string, std::string> keywords;
                            keywords["PORT"] = pi->name;
                            CommandManager::INSTANCE.TriggerPreset("EFUSE_TRIGGERED", keywords);
                        }
                    }
                    return true;
                });
            }
            hasInfo = true;
        }
    }
    if (pinConfig.isMember("currentSensor")) {
        hasInfo = true;
        pi->currentMonitorScale = pinConfig["currentSensor"]["scale"].asFloat();
        pi->currentMonitorFile = open(pinConfig["currentSensor"]["path"].asString().c_str(), O_NONBLOCK | O_RDONLY);
    }

    if (hasInfo) {
        portPins.push_back(pi);
    } else {
        delete pi;
    }
}

const PinCapabilities * OutputMonitor::AddOutputPin(const std::string &name, const std::string &pinName) {
    std::string pin = pinName;
    bool highToEnable = true;
    if (pin[0] == '!') {
        pin = pin.substr(1);
        highToEnable = false;
    }
    auto &op = highToEnable ? pullHighOutputPins : pullLowOutputPins;
    for (auto &pc : op) {
        if (pc->name == pin) {
            return pc;
        }
    }

    const PinCapabilities *pc = PinCapabilities::getPinByName(pin).ptr();
    if (!pc) {
        LogWarn(VB_CHANNELOUT, "Could not find pin " + pin + " to enable output " + name + "\n");
        WarningHolder::AddWarning("Could not find pin " + pin + " to enable output " + name);
        return nullptr;
    }
    op.push_back(pc);
    pc->configPin("gpio", true);
    pc->setValue(!highToEnable);
    return pc;
}

void OutputMonitor::AutoEnableOutputs() {
    int i = getSettingInt("AutoEnableOutputs", 1);
    if (i) {
        EnableOutputs();
    }
}
void OutputMonitor::AutoDisableOutputs() {
    int i = getSettingInt("AutoEnableOutputs", 1);
    if (i) {
        DisableOutputs();
    }
}


const std::shared_ptr<httpserver::http_response> OutputMonitor::render_GET(const httpserver::http_request& req) {
    int plen = req.get_path_pieces().size();
    if (plen > 1 && req.get_path_pieces()[1] == "ports") {
        if (portPins.empty()) {
            return std::shared_ptr<httpserver::http_response>(new httpserver::string_response("[]", 200, "application/json"));
        }

        Json::Value result;
        for (auto a : portPins) {
            a->appendTo(result);
        }
        std::string resultStr = SaveJsonToString(result);
        return std::shared_ptr<httpserver::http_response>(new httpserver::string_response(resultStr, 200, "application/json"));
    }
    return std::shared_ptr<httpserver::http_response>(new httpserver::string_response("Not Found", 404, "text/plain"));
}