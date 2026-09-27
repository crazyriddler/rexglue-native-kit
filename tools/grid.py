import sys
from PIL import Image
out=sys.argv[1]; files=sys.argv[2:]; cols=4
ims=[Image.open(f).resize((480,270)) for f in files]
rows=(len(ims)+cols-1)//cols
W=Image.new('RGB',(480*cols,270*rows))
for i,im in enumerate(ims): W.paste(im,((i%cols)*480,(i//cols)*270))
W.save(out)
