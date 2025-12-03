# GPT4all User Guide

## Overview
This application allows users to ask questions from the text user provided. This functionality achieves quick responses from the LLM, enhancing the overall user experience.

---
## Chapter 1: Installation and Setting

### Installation Steps

```bash
git clone <repository-url>
cd aiDAPTIV-Integration-gpt4all
cd build
cmake ..
make
./main
```

---

## Chapter 2: How to Use?

### Usage Workflow

1. **Initial Setup**
- Click on `chat.exe` to launch the chat interface. The chat room automatically created. 
![image](img/fig_1.PNG)

2. **Set LLM Endpoint**
- Fill in the **LLM Endpoint** and **Model Name** as required.
![image](img/fig_2.PNG)
![image](img/fig_3.PNG)

3. **Set RAG**
- Add a collection and set the folder path. It will automatically pull all the files from the folder path and insert to the collection.
- Please wait for the Ready sign of all collections.
![image](img/fig_4.PNG)
![image](img/fig_5.PNG)

4. **Ask questions**
- Select the collection on the right side. You can start asking questions about the texts you uploaded.
![image](img/fig_6.PNG)