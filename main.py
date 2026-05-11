import asyncio
import signal
from kuksa_client.grpc.aio import VSSClient
from kuksa_client.grpc import Datapoint


class SmartWiperApp:

    def __init__(self, client: VSSClient):
        self.client = client
        self.hood_open = False
        self.current_wiper_mode = "OFF"
        self.running = True

    async def initialize_system(self):
        await self.client.set_current_values({
            "Vehicle.Body.Hood.IsOpen": Datapoint(False),
            "Vehicle.Body.Windshield.Front.Wiping.Mode": Datapoint("OFF")
        })

        self.hood_open = False
        self.current_wiper_mode = "OFF"

        print("System initialized (Hood=False, Wipers=OFF)")

    async def reset_system(self):
        print("Resetting signals to default state...")

        await self.client.set_current_values({
            "Vehicle.Body.Hood.IsOpen": Datapoint(False),
            "Vehicle.Body.Windshield.Front.Wiping.Mode": Datapoint("OFF")
        })

        print("System reset complete.")

    async def on_start(self):
        print("Smart Wiper Safety System Started")

        await self.initialize_system()

        await self.listener()

    async def listener(self):

        async for update in self.client.subscribe_current_values([
            "Vehicle.Body.Hood.IsOpen",
            "Vehicle.Body.Windshield.Front.Wiping.Mode"
        ]):

            if not self.running:
                break

            # -------------------------
            # HOOD STATE
            # -------------------------
            if "Vehicle.Body.Hood.IsOpen" in update:

                hood_datapoint = update["Vehicle.Body.Hood.IsOpen"]

                if hood_datapoint is None:
                    continue

                self.hood_open = hood_datapoint.value
                print(f"Hood state changed: {self.hood_open}")

                # Safety: if hood opens, immediately force wipers OFF
                if self.hood_open and self.current_wiper_mode != "OFF":
                    print("Hood opened while wipers active.")
                    print("Turning OFF wipers for safety...")

                    await self.client.set_current_values({
                        "Vehicle.Body.Windshield.Front.Wiping.Mode": Datapoint("OFF")
                    })

                    self.current_wiper_mode = "OFF"

            # -------------------------
            # WIPER MODE
            # -------------------------
            if "Vehicle.Body.Windshield.Front.Wiping.Mode" in update:

                wiper_datapoint = update["Vehicle.Body.Windshield.Front.Wiping.Mode"]

                if wiper_datapoint is None:
                    continue

                requested_mode = wiper_datapoint.value

                # HARD SAFETY RULE
                if self.hood_open:

                    if requested_mode != "OFF":
                        print("Hood is open → Wipers cannot be activated")

                        await self.client.set_current_values({
                            "Vehicle.Body.Windshield.Front.Wiping.Mode": Datapoint("OFF")
                        })

                    self.current_wiper_mode = "OFF"
                    continue

                # Accept request only if hood is closed
                if self.current_wiper_mode != requested_mode:
                    self.current_wiper_mode = requested_mode
                    print(f"Wiper mode set to: {self.current_wiper_mode}")


async def main():
    client = VSSClient("localhost", 55555)
    await client.connect()

    app = SmartWiperApp(client)

    loop = asyncio.get_running_loop()
    shutdown_event = asyncio.Event()

    def handle_signal():
        shutdown_event.set()

    loop.add_signal_handler(signal.SIGINT, handle_signal)
    loop.add_signal_handler(signal.SIGTERM, handle_signal)

    main_task = asyncio.create_task(app.on_start())

    # Wait for shutdown signal
    await shutdown_event.wait()

    print("Shutting down Smart Wiper App...")
    app.running = False
    main_task.cancel()

    try:
        await main_task
    except asyncio.CancelledError:
        pass

    # This now runs cleanly after the main task is cancelled
    await app.reset_system()


if __name__ == "__main__":
    asyncio.run(main())