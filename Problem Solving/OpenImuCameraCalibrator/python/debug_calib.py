
import numpy as np
import matplotlib.pyplot as plt
import os
base_path = ""

def read_file(base_path, file):
    with open(os.path.join(base_path,file)) as f:
        lines = f.readlines() 
        array = np.zeros((len(lines),3))
        # Strips the newline character 
        for i, line in enumerate(lines): 
            data = line.split()
            for c in range(3):
                array[i,c] = float(data[c+1])
    # read txt
    return array


vis_q = read_file(base_path, "visual_gyroscope_all.txt")
vis_acc = read_file(base_path, "visual_gyroscope.txt")
gyr_trafo = read_file(base_path, "gyroscope_transformed.txt")

#plt.plot(vis_q[:,0], 'r')
plt.plot(vis_acc[:,0],'g')
plt.plot(gyr_trafo[:,0],'b--')
plt.show()

#plt.plot(vis_q[:,1], 'r')
plt.plot(vis_acc[:,1],'g')
plt.plot(gyr_trafo[:,1],'b--')
plt.show()

#plt.plot(vis_q[:,2], 'r')
plt.plot(vis_acc[:,2],'g')
plt.plot(gyr_trafo[:,2],'b--')
plt.show()
