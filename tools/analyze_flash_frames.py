"""Inspect bounded P5R recordings; rank brief visual changes, not presumed bugs."""
from pathlib import Path
import argparse,csv,struct
import numpy as np

W,H,C=192,108,4
SIZE=64+W*H*C*4
def bmp(path,rgba,alpha=False):
    data=rgba.copy()
    if alpha: data[:,:,:3]=data[:,:,3,None]
    data=data[:,:,[2,1,0,3]];data[:,:,3]=255
    h,w=data.shape[:2]
    header=struct.pack('<2sIHHI',b'BM',54+data.nbytes,0,0,54)
    header+=struct.pack('<IiiHHIIiiII',40,w,-h,1,32,0,data.nbytes,2835,2835,0,0)
    path.write_bytes(header+data.tobytes())
def analyze(path,out,top,frame):
    n=path.stat().st_size//SIZE
    if not n: raise SystemExit('No complete records saved.')
    raw=np.memmap(path,dtype=np.uint8,mode='r',shape=(n,SIZE))
    headers=np.array(raw[:,:64]).view('<u4').reshape(n,16)
    if not np.all(headers[:,0]==0x46523550) or not np.all(headers[:,1:4]==[1,W,H]):raise SystemExit('Unknown capture format')
    order=np.argsort(headers[:,14]);scores=[];previous=None;current=None
    out.mkdir(parents=True,exist_ok=True)
    for pos,idx in enumerate(order):
        rgba=np.array(raw[idx,64:]).reshape(C,H,W,4)
        # Keep premultiplied UI colour AND alpha: invisible RGB is not a screen flash.
        pixels=rgba.astype(np.int16);pixels[2:,:,:,:3]=np.where(pixels[2:,:,:,3,None]>0,pixels[2:,:,:,:3],0)
        if pos>=2:
            delta1=np.abs(current-previous).mean(axis=(1,2,3));delta2=np.abs(current-pixels).mean(axis=(1,2,3))
            score=np.minimum(delta1,delta2)
            j=order[pos-1]
            # A dropped frame breaks the consecutive evidence; mark instead of treating it as a flash.
            continuous=int(headers[j,14]==headers[order[pos-2],14]+1 and headers[idx,14]==headers[j,14]+1 and headers[j,4]==headers[order[pos-2],4]+1 and headers[idx,4]==headers[j,4]+1)
            scores.append((int(j),int(headers[j,4]),continuous,*map(float,score)))
        previous=current;current=pixels
    with (out/'changes.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow(['record','present_frame','consecutive','left_change','right_change','hud_change','overlay_change']);writer.writerows(scores)
    selected=set()
    if frame is not None:
        selected.update(int(i) for i in order if int(headers[i,4])==frame)
    else:
        for channel in (3,4,5,6):
            selected.update(r[0] for r in sorted((r for r in scores if r[2]),key=lambda r:r[channel],reverse=True)[:top])
    # Neighbours are necessary to distinguish cuts/animation from one-frame anomalies.
    positions={int(idx):pos for pos,idx in enumerate(order)}
    selected={int(order[p]) for i in selected for p in range(max(0,positions[i]-1),min(n,positions[i]+2))}
    for idx in sorted(selected):
        rgba=np.array(raw[idx,64:]).reshape(C,H,W,4);h=headers[idx]
        for c,name in enumerate(('left','right','hud','overlay')):
            if h[5]&(1<<c):
                bmp(out/f'f{h[4]:06d}_{name}.bmp',rgba[c])
                if c>=2:bmp(out/f'f{h[4]:06d}_{name}_alpha.bmp',rgba[c],True)
    incomplete=int(np.count_nonzero((headers[:,5]&3)!=3))
    failed=int(np.count_nonzero((headers[:,6]!=1)|(headers[:,7].view('<i4')<0)))
    print(f'{n} records; {path.stat().st_size%SIZE} trailing bytes; {incomplete} missing eye sources; {failed} unsuccessful stereo/end submissions.')
    print(f'Present frames {headers[order[0],4]}..{headers[order[-1],4]}; exported {len(selected)} frames to {out}.')
    print('Change scores are inspection candidates, not identified flash/fog defects. Sources precede XR copies; metadata records stereo/end success.')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--out',type=Path);p.add_argument('--top',type=int,default=4);p.add_argument('--frame',type=int)
    a=p.parse_args();analyze(a.capture,a.out or a.capture.parent/'analysis',a.top,a.frame)
