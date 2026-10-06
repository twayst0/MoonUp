# Renders the UI in headless Chromium (mock backend) and saves screenshots for review.
import asyncio, sys, subprocess, time, os
from playwright.async_api import async_playwright
ROOT = os.path.join(os.path.dirname(__file__), '..', 'ui')
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/shots'
os.makedirs(OUT, exist_ok=True)
srv = subprocess.Popen([sys.executable, '-m', 'http.server', '8765', '-d', ROOT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.8)
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch()
        pg = await b.new_page(viewport={'width': 1240, 'height': 800}, device_scale_factor=1)
        logs = []
        pg.on('console', lambda m: logs.append(f'{m.type}: {m.text}'))
        pg.on('pageerror', lambda e: logs.append(f'PAGEERROR: {e}'))
        await pg.goto('http://localhost:8765/index.html')
        for ms in [int(x) for x in os.environ.get('INTRO_SHOTS', '250,900,1600,2500').split(',') if x]:
            await pg.wait_for_timeout(ms if ms < 400 else 0)
        # timed intro shots
        await pg.reload()
        t0 = time.time()
        for ms in [int(x) for x in os.environ.get('INTRO_SHOTS', '250,900,1600,2500').split(',') if x]:
            now = (time.time() - t0) * 1000
            if ms > now: await pg.wait_for_timeout(ms - now)
            await pg.screenshot(path=f'{OUT}/intro_{ms}.png')
        await pg.wait_for_selector('.modal-back', timeout=15000)
        await pg.wait_for_timeout(600)
        await pg.screenshot(path=f'{OUT}/onboarding.png')
        await pg.keyboard.press('Escape')
        await pg.wait_for_timeout(700)
        pages = os.environ.get('PAGES', 'home,scaling,render,motion,vision,profiles,advisor,lab,settings,about').split(',')
        for name in pages:
            await pg.click(f'.nav-item[data-page="{name}"]')
            await pg.wait_for_timeout(int(os.environ.get('WAIT','900')))
            await pg.screenshot(path=f'{OUT}/page_{name}.png')
        if os.environ.get('SCALE'):
            await pg.click('.nav-item[data-page="home"]'); await pg.wait_for_timeout(400)
            await pg.click('#engage'); await pg.wait_for_timeout(1500)
            await pg.screenshot(path=f'{OUT}/home_countdown.png')
            await pg.wait_for_timeout(6500)
            await pg.screenshot(path=f'{OUT}/home_running.png')
        print('\n'.join(logs[-30:]))
        await b.close()
asyncio.run(main())
srv.terminate()
