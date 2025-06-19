MCP Protocol IoT Control Usage Instructions


> This document introduces how to implement IoT control of ESP32 devices based on the MCP protocol. For detailed protocol flow, please refer to [`mcp-protocol.md`](./mcp-protocol.md).

## Introduction

MCP (Model Context Protocol) is a next-generation protocol recommended for IoT control, which discovers and calls "Tools" between backend and devices through standard JSON-RPC 2.0 format, enabling flexible device control.

## Typical Usage Flow

1. After device startup, establish connection with backend through basic protocols (such as WebSocket/MQTT).

2. Backend initializes session through MCP protocol's `initialize` method.

3. Backend retrieves all tools (functions) and parameter descriptions supported by the device through `tools/list`.

4. Backend calls specific tools through `tools/call` to achieve device control.

For detailed protocol format and interaction, please see [`mcp-protocol.md`](./mcp-protocol.md).

## Device-side Tool Registration Method Description

Devices register "tools" that can be called by the backend through the `McpServer::AddTool` method. Its common function signature is as follows:

```cpp
void AddTool(
    const std::string& name,           // Tool name, recommended to be unique and hierarchical, e.g. self.dog.forward
    const std::string& description,    // Tool description, concise explanation of functionality for AI model understanding
    const PropertyList& properties,    // Input parameter list (can be empty), supported types: boolean, integer, string
    std::function<ReturnValue(const PropertyList&)> callback // 工Callback implementation when tool is called
);
```

- name: Unique tool identifier, recommended to use "module.function" naming convention.

- description: Natural language description for AI/user understanding.

- properties: Parameter list, supported types are boolean, integer, string, with configurable ranges and default values.

- callback: Actual execution logic when receiving call requests, return value can be bool/int/string.

## Typical Registration Example (Using ESP-Hi as Example)

```cpp
void InitializeTools() {
    auto& mcp_server = McpServer::GetInstance();
    // Example 1: No parameters, control robot to move forward
    mcp_server.AddTool("self.dog.forward", "Robot moves forward", PropertyList(), [this](const PropertyList&) -> ReturnValue {
        servo_dog_ctrl_send(DOG_STATE_FORWARD, NULL);
        return true;
    });

    // Example 2: With parameters, set light RGB color
    mcp_server.AddTool("self.light.set_rgb", "Set RGB color", PropertyList({
        Property("r", kPropertyTypeInteger, 0, 255),
        Property("g", kPropertyTypeInteger, 0, 255),
        Property("b", kPropertyTypeInteger, 0, 255)
    }), [this](const PropertyList& properties) -> ReturnValue {
        int r = properties["r"].value<int>();
        int g = properties["g"].value<int>();
        int b = properties["b"].value<int>();
        led_on_ = true;
        SetLedColor(r, g, b);
        return true;
    });
}
```

## Common Tool Call JSON-RPC Examples

### 1. Get Tool List

### 2. Control Chassis to Move Forward

### 3. Switch Light Mode

### 4. Camera Flip

## Notes

- Tool names, parameters and return values should be based on device-side `AddTool` registration.

- It is recommended that all new projects uniformly adopt MCP protocol for IoT control.

- For detailed protocol and advanced usage, please refer to [`mcp-protocol.md`](./mcp-protocol.md). 