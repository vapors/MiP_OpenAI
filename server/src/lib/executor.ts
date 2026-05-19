import { StructuredTool } from "@langchain/core/tools";

class VoiceToolExecutor {
    protected toolsByName: Record<string, StructuredTool>;
    protected triggerPromise: Promise<any> | null = null;
    protected triggerResolve: ((value: any) => void) | null = null;
    protected lock: Promise<void> | null = null;

    constructor(toolsByName: Record<string, StructuredTool>) {
        this.toolsByName = toolsByName;
    }

    protected async triggerFunc(): Promise<any> {
        if (!this.triggerPromise) {
            this.triggerPromise = new Promise((resolve) => {
                this.triggerResolve = resolve;
            });
        }
        return this.triggerPromise;
    }

    async addToolCall(toolCall: any): Promise<void> {
        while (this.lock) {
            await this.lock;
        }

        this.lock = (async () => {
            if (this.triggerResolve) {
                this.triggerResolve(toolCall);
                this.triggerPromise = null;
                this.triggerResolve = null;
            } else {
                throw new Error("Tool call adding already in progress");
            }
        })();

        await this.lock;
        this.lock = null;
    }

    protected async createToolCallTask(toolCall: any): Promise<any> {
        const tool = this.toolsByName[toolCall.name];
        if (!tool) {
            throw new Error(
                `Tool ${toolCall.name} not found. Must be one of ${Object.keys(
                    this.toolsByName
                )}`
            );
        }

        let args;
        try {
            args = JSON.parse(toolCall.arguments);
        } catch (error) {
            throw new Error(
                `Failed to parse arguments '${toolCall.arguments}'. Must be valid JSON.`
            );
        }

        const result = await tool.call(args);
        const resultStr =
            typeof result === "string" ? result : JSON.stringify(result);

        return {
            type: "conversation.item.create",
            item: {
                id: toolCall.call_id,
                call_id: toolCall.call_id,
                type: "function_call_output",
                output: resultStr,
            },
        };
    }

    async *outputIterator(): AsyncGenerator<any, void, unknown> {
        while (true) {
            const toolCall = await this.triggerFunc();
            try {
                const result = await this.createToolCallTask(toolCall);
                yield result;
            } catch (error: any) {
                yield {
                    type: "conversation.item.create",
                    item: {
                        id: toolCall.call_id,
                        call_id: toolCall.call_id,
                        type: "function_call_output",
                        output: `Error: ${error.message}`,
                    },
                };
            }
        }
    }
}

export { VoiceToolExecutor };