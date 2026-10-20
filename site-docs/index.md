# InspIRCd JavaScript module

Embed trusted administrator-authored JavaScript and synchronous WebAssembly in
InspIRCd **4.12.1**. The optional module exposes all **75 core Module On* hooks**,
**22 separate event-provider callbacks**, both command-validation phases,
five explicit mutation-patch adapters, and six audited native utilities. It is **not a hostile-code sandbox**.
The ordinary image is unchanged and the module is not loaded by default.

Choose the documentation that fits your task:

| Need | Start here |
| --- | --- |
| Learn by doing in a disposable local server | [Your first command hook](tutorials/first-hook.md) |
| Accomplish a specific task | [Build and configure](how-to/configure.md), [reload](how-to/reload.md), [Wasm](how-to/wasm.md), [tests](how-to/test.md) |
| Look up exact behavior | [Configuration](reference/configuration.md), [command events](reference/commands.md), [core hooks and patches](reference/hooks.md), [event-provider hooks](reference/events.md), [API](reference/api.md), [limits](reference/limits.md) |
| Understand the design | [Trust and containment](explanation/trust.md), [snapshots and effects](explanation/architecture.md) |

This site follows [Diátaxis](https://diataxis.fr/start-here/): lessons teach skills,
how-to guides solve tasks, reference specifies behavior, and explanation supplies context.
It describes this source checkout, not a deployment or claim that every published
release contains these features. Event subscriptions are not full native API parity;
see the event-provider reference for observation-only and unavailable operations.
