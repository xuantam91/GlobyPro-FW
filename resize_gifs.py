import os
import glob
from PIL import Image

def process_gif(input_path, output_path):
    print(f"Processing {input_path}...")
    with Image.open(input_path) as im:
        frames = []
        durations = []
        try:
            while True:
                duration = im.info.get('duration', 100)
                durations.append(duration)
                
                # Use RGBA to handle transparency correctly
                frame = im.convert('RGBA')
                
                # Resize from 240x240 to 160x160 (maintaining aspect ratio)
                resized_frame = frame.resize((160, 160), Image.Resampling.LANCZOS)
                
                # Create a new 240x160 black canvas
                new_frame = Image.new('RGB', (240, 160), (0, 0, 0))
                
                # Paste the resized frame horizontally centered
                # x = (240 - 160) // 2 = 40
                # y = 0
                new_frame.paste(resized_frame, (40, 0), resized_frame)
                
                frames.append(new_frame)
                im.seek(im.tell() + 1)
        except EOFError:
            pass
        
        loop = im.info.get('loop', 0)
        
        # Save the new GIF, optimizing for size
        frames[0].save(output_path,
                       save_all=True,
                       append_images=frames[1:],
                       loop=loop,
                       duration=durations,
                       optimize=True)
        print(f"Saved to {output_path}")

input_dir = "./managed_components/txp666__otto-emoji-gif-component/gifs"
output_dir = "./gifs_240x160"
os.makedirs(output_dir, exist_ok=True)

for file in glob.glob(os.path.join(input_dir, "*.gif")):
    filename = os.path.basename(file)
    output_path = os.path.join(output_dir, filename)
    process_gif(file, output_path)

print("Done processing all GIFs.")
