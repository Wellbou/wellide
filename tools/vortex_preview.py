import math,sys
def vortex(N,rot=0.0,arms=4,twist=1.5,thick=0.55,eye=1.0,clean=True):
    c=(N-1)/2; g=[[0]*N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            dx,dy=x-c,y-c; r=math.hypot(dx,dy)
            if r>N/2-0.3: continue
            if r<eye: g[y][x]=3; continue
            th=math.atan2(dy,dx)+rot
            seg=(arms*th/(2*math.pi)-twist*math.log(r+1))%arms
            i=int(seg); f=seg-i
            if f<thick: g[y][x]=1 if i%2==0 else 2
    if clean:  # drop lonely pixels: pixel art hates specks
        for y in range(N):
            for x in range(N):
                v=g[y][x]
                if v in (1,2):
                    nb=sum(1 for dx,dy in((1,0),(-1,0),(0,1),(0,-1)) if 0<=x+dx<N and 0<=y+dy<N and g[y+dy][x+dx]==v)
                    if nb==0: g[y][x]=0
    return g
def show(g): print("\n".join(" ".join(".#@o"[v] for v in row) for row in g))
if __name__=="__main__":
    N=int(sys.argv[1]); tw=float(sys.argv[2]); th=float(sys.argv[3]); eye=float(sys.argv[4]) if len(sys.argv)>4 else 1.0
    show(vortex(N,twist=tw,thick=th,eye=eye))
