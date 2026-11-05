"""Decode per-draw GPU crops and rank visible draw contributions; no game access."""
from pathlib import Path
import argparse,json,re,struct,zlib
import numpy as np

def png(path,rgb):
    h,w,_=rgb.shape
    def chunk(name,payload):
        return struct.pack('>I',len(payload))+name+payload+struct.pack('>I',zlib.crc32(name+payload)&0xffffffff)
    rows=b''.join(b'\0'+row.tobytes() for row in rgb)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))

def analyze(folder):
    results=[]
    for meta in sorted(folder.glob('*_meta.txt')):
        s=meta.read_text();prefix=meta.name.split('_')[0]
        if 'complete=1' not in s:continue
        match=re.search(r'crop=(\d+),(\d+),(\d+),(\d+) format=(\d+)',s)
        x,y,w,h,fmt=map(int,match.groups())
        images=[]
        for i,name in enumerate(('before','after')):
            pitch=int(re.search(fr'image{i} rowPitch=(\d+)',s)[1])
            data=np.frombuffer((folder/f'{prefix}_{name}.raw').read_bytes(),dtype=np.uint8).reshape(h,pitch)
            if fmt in (28,29,87):
                rgba=data[:,:w*4].copy().reshape(h,w,4).astype(np.float32)/255
                if fmt==87:rgba=rgba[:,:,[2,1,0,3]]
            elif fmt in (10,2):
                dtype=np.dtype('<f2' if fmt==10 else '<f4')
                rgba=data[:,:w*4*dtype.itemsize].copy().view(dtype).reshape(h,w,4).astype(np.float32)
            else:raise ValueError(f'Unsupported format {fmt}')
            images.append(rgba)
            rgb=np.nan_to_num(rgba[:,:,:3],nan=0,posinf=1,neginf=0)
            png(folder/f'{prefix}_{name}.png',(np.clip(rgb,0,1)*255).astype('uint8'))
        delta=images[1][:,:,:3]-images[0][:,:,:3]
        valid=np.isfinite(delta).all(axis=2)
        changed=np.max(np.abs(np.nan_to_num(delta)),axis=2)>1/255
        heat=np.clip(np.abs(np.nan_to_num(delta))*8,0,1)
        png(folder/f'{prefix}_difference.png',(heat*255).astype('uint8'))
        results.append(dict(sample=prefix,frame=int(re.search(r'frame=(\d+)',s)[1]),pixel_shader=re.search(r'PS=(\S+)',s)[1],changed_fraction=float(changed.mean()),mean_rgb_change=np.nan_to_num(delta).mean(axis=(0,1)).tolist(),finite_fraction=float(valid.mean())))
    results.sort(key=lambda r:r['changed_fraction'],reverse=True)
    (folder/'analysis.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(results[:12],indent=2))
    return results
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);args=parser.parse_args();analyze(args.folder)
